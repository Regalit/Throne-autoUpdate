#include "include/ui/shadowlos/ShadowlosChrome.hpp"

// Brings in ui_mainwindow.h, i.e. the full Ui::MainWindow. Only when MW_INTERFACE is unset,
// which is why CMake keeps this file out of unity batches.
#include "include/ui/mainwindow.h"

#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/SettingsRepo.h"
#include "include/ui/setting/ThemeManager.hpp"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/widget/StartStopButton.hpp"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QTimer>
#include <algorithm>
#include <QToolButton>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <QLibrary>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif
#endif

// Everything file-local lives in this uniquely named namespace: Windows builds are unity
// builds, where an anonymous namespace is shared with whichever .cpp files land in the same batch.
namespace Shadowlos::ChromeImpl {

    QColor hex(QRgb rgb) {
        return QColor::fromRgb(rgb);
    }

    QString translate(const char *text) {
        return QCoreApplication::translate("ShadowlosChrome", text);
    }

    // Sheet rules keyed on slRole or an objectName only reach a widget the style has
    // already polished once it is polished again.
    void repolish(QWidget *widget) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }

    QIcon icon(const char *name) {
        return QIcon(QStringLiteral(":/shadowlos/icons/%1.png").arg(QLatin1String(name)));
    }

    // ------------------------------------------------------------ title bars

#ifdef Q_OS_WIN
    // Recolours the native caption to the theme's black. Resolved at runtime so the build
    // needs neither <windows.h> (unity builds) nor an extra link to dwmapi; on Windows
    // older than 11 build 22000 the colour attributes are simply rejected.
    void applyCaption(QWidget *window) {
        using SetWindowAttribute = long(__stdcall *)(void *, unsigned long, const void *, unsigned long);
        static const auto setAttribute = reinterpret_cast<SetWindowAttribute>(
            QLibrary::resolve(QStringLiteral("dwmapi"), "DwmSetWindowAttribute"));
        if (setAttribute == nullptr || window == nullptr || !window->testAttribute(Qt::WA_WState_Created)) return;
        const Qt::WindowType type = window->windowType();
        if (type != Qt::Window && type != Qt::Dialog) return;

        constexpr unsigned long kUseImmersiveDarkMode = 20;
        constexpr unsigned long kBorderColor = 34;
        constexpr unsigned long kCaptionColor = 35;
        constexpr unsigned long kTextColor = 36;
        constexpr unsigned long kColorDefault = 0xFFFFFFFFul;

        const bool active = Shadowlos::Chrome::ThemeActive();
        int dark = active ? 1 : 0;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (!active) dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? 1 : 0;
#endif
        // COLORREF is 0x00BBGGRR.
        const unsigned long caption = active ? 0x000F0D0Bul : kColorDefault; // #0B0D0F
        const unsigned long text = active ? 0x00FFFFFFul : kColorDefault;
        const unsigned long border = active ? 0x0023201Aul : kColorDefault;  // #1A2023

        void *hwnd = reinterpret_cast<void *>(window->winId());
        setAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
        setAttribute(hwnd, kCaptionColor, &caption, sizeof(caption));
        setAttribute(hwnd, kTextColor, &text, sizeof(text));
        setAttribute(hwnd, kBorderColor, &border, sizeof(border));
    }
#else
    void applyCaption(QWidget *) {}
#endif

    void applyCaptionToVisibleWindows() {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->isVisible()) applyCaption(w);
        }
    }

    // Application-wide: rounded menus need a translucent window, and every new window or
    // dialog needs its caption recoloured.
    class AppFilter final : public QObject {
    public:
        using QObject::QObject;

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            switch (event->type()) {
                case QEvent::Polish:
                    // A QMenu is polished while sizing itself, before its native window exists;
                    // translucency can only be requested before that.
                    if (auto *menu = qobject_cast<QMenu *>(watched);
                        menu != nullptr && !menu->testAttribute(Qt::WA_WState_Created) &&
                        Shadowlos::Chrome::ThemeActive()) {
                        menu->setAttribute(Qt::WA_TranslucentBackground, true);
                    }
                    break;
                case QEvent::Show:
                    if (auto *w = qobject_cast<QWidget *>(watched); w != nullptr && w->isWindow()) {
                        applyCaption(w);
                    }
                    break;
                default:
                    break;
            }
            return false;
        }
    };

    // ---------------------------------------------------------- power button

    // The design's connect button: a ring and a disc, blue while connected. Takes over
    // StartStopButton's painting only; state, tooltips and animations stay the base class's.
    class PowerButton final : public StartStopButton {
    public:
        explicit PowerButton(QWidget *parent) : StartStopButton(parent) {
            setAttribute(Qt::WA_Hover, true);
            setFixedSize(64, 64);
        }

        QSize sizeHint() const override { return {64, 64}; }

    protected:
        void enterEvent(QEnterEvent *event) override {
            StartStopButton::enterEvent(event);
            update();
        }

        void leaveEvent(QEvent *event) override {
            StartStopButton::leaveEvent(event);
            update();
        }

        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing, true);

            const State st = state();
            const bool running = st == State::Running;
            const bool busy = st == State::Connecting || st == State::Disconnecting;
            const bool hovered = underMouse() && isEnabled();

            const qreal side = qMin(width(), height()) - 2.0;
            const QPointF c = QRectF(rect()).center();
            const qreal scale = 1.0 - 0.05 * press();
            p.translate(c);
            p.scale(scale, scale);
            p.translate(-c);
            p.setOpacity(dim());

            // Ring: 64px in the design, 1.33px accent outline.
            const QRectF outer(c.x() - side / 2, c.y() - side / 2, side, side);
            QLinearGradient ring(outer.topLeft(), outer.bottomLeft());
            ring.setColorAt(0, running ? hex(0x0072CC) : hex(0x111517));
            ring.setColorAt(1, running ? hex(0x1A374D) : hex(0x1A2023));
            QColor outline = running || hovered ? hex(0x33A5FF) : hex(0x2E3438);
            if (!running && hovered) outline.setAlphaF(0.6);
            p.setPen(QPen(outline, 1.33));
            p.setBrush(ring);
            p.drawEllipse(outer.adjusted(0.67, 0.67, -0.67, -0.67));

            // Disc: 57px in the design.
            const qreal inset = side * 3.5 / 64.0;
            const QRectF inner = outer.adjusted(inset, inset, -inset, -inset);
            QLinearGradient disc(inner.topLeft(), inner.bottomLeft());
            disc.setColorAt(0, running ? hex(0x33A5FF) : hex(0x0B0D0F));
            disc.setColorAt(1, running ? hex(0x008FFF) : hex(0x111517));
            p.setPen(Qt::NoPen);
            p.setBrush(disc);
            p.drawEllipse(inner);

            if (busy) {
                QPen arc(hex(0x33A5FF), qMax(2.0, side * 0.045));
                arc.setCapStyle(Qt::RoundCap);
                p.setPen(arc);
                p.setBrush(Qt::NoBrush);
                const qreal a = arc.widthF() / 2 + 0.5;
                p.drawArc(outer.adjusted(a, a, -a, -a), static_cast<int>(-spin() * 16), -100 * 16);
            }

            QColor glyph;
            if (running) glyph = Qt::white;
            else if (busy) glyph = hex(0x33A5FF);
            else if (st == State::Disabled) glyph = hex(0x474D53);
            else glyph = hovered ? QColor(Qt::white) : hex(0xC7CDD1);

            // Power symbol, 28px in the design: an open ring with a stroke through the gap.
            const qreal g = side * 28.0 / 64.0;
            const qreal r = g * 0.36;
            QPen pen(glyph, qMax(1.6, g * 0.1));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(c.x() - r, c.y() - r + g * 0.05, 2 * r, 2 * r), (90 + 38) * 16, (360 - 76) * 16);
            p.drawLine(QPointF(c.x(), c.y() - g * 0.42), QPointF(c.x(), c.y() - g * 0.02));
        }
    };

    // --------------------------------------------------------- profile table

    // ProfilesTableModel hands out Qt's stock dark colours for latency; on the design's dark
    // rows those read poorly, so they are swapped for the design's status colours.
    QColor designLatencyColor(const QColor &c) {
        switch (c.rgb() & 0xFFFFFFu) {
            case 0x008000u: return hex(0x86D75E); // Qt::darkGreen: fast
            case 0x808000u: return hex(0xFFC633); // Qt::darkYellow: slow
            case 0xFF0000u: return hex(0xFF453A); // Qt::red: very slow
            case 0x008080u: return hex(0x33A5FF); // Qt::darkCyan: connect-only
            case 0x808080u: return hex(0x5E676E); // Qt::darkGray: failed
            default: return {};
        }
    }

    bool isRunningRow(const QModelIndex &index) {
        if (Configs::dataManager == nullptr || !Configs::dataManager->settingsRepo) return false;
        const int started = Configs::dataManager->settingsRepo->started_id;
        if (started < 0) return false;
        const QVariant id = index.data(ProfilesTableModel::ProfileIdRole);
        return id.isValid() && id.toInt() == started;
    }

    // Paints the connected profile's row as the design's highlighted row.
    constexpr int kSimpleFontPx = 17;

    class RowDelegate final : public QStyledItemDelegate {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
            const bool running = Shadowlos::Chrome::ThemeActive() && isRunningRow(index);
            if (running) painter->fillRect(option.rect, hex(0x1A374D));
            QStyledItemDelegate::paint(painter, option, index);
            if (!running) return;

            // After the base paint, which draws the sheet's row separator over the bottom edge.
            painter->save();
            painter->setPen(QPen(hex(0x33A5FF), 1));
            const QRect r = option.rect.adjusted(0, 0, -1, -1);
            painter->drawLine(r.topLeft(), r.topRight());
            painter->drawLine(r.bottomLeft(), r.bottomRight());
            if (index.column() == 0) painter->drawLine(r.topLeft(), r.bottomLeft());
            if (index.model() != nullptr && index.column() == index.model()->columnCount(index.parent()) - 1) {
                painter->drawLine(r.topRight(), r.bottomRight());
            }
            painter->restore();
        }

    protected:
        void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
            QStyledItemDelegate::initStyleOption(option, index);
            // The simple view's bigger rows. Set here, not in the sheet: a re-polish does not
            // re-apply a sheet font to a view that is already shown.
            if (parent() != nullptr && parent()->property("slSimple").toBool()) option->font.setPixelSize(kSimpleFontPx);
            if (!Shadowlos::Chrome::ThemeActive()) return;
            const QVariant foreground = index.data(Qt::ForegroundRole);
            if (!foreground.isValid()) return;
            const QColor mapped = designLatencyColor(foreground.value<QColor>());
            if (mapped.isValid()) option->palette.setBrush(QPalette::Text, mapped);
        }
    };

    // --------------------------------------------------------------- toggles

    // Keeps a toggle's label in step with its switch: a click on the text flips it, and
    // hiding the switch (System DNS is hidden unless enabled in settings) hides the row.
    class ToggleLink final : public QObject {
    public:
        ToggleLink(QCheckBox *box, QWidget *row, QLabel *label) : QObject(box), m_box(box), m_row(row), m_label(label) {
            box->installEventFilter(this);
            label->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (m_box.isNull() || m_row.isNull() || m_label.isNull()) return false;
            if (watched == m_label.data() && event->type() == QEvent::MouseButtonRelease) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton && m_box->isEnabled()) m_box->click();
            } else if (watched == m_box.data()) {
                switch (event->type()) {
                    case QEvent::HideToParent: m_row->hide(); break;
                    case QEvent::ShowToParent: m_row->show(); break;
                    case QEvent::EnabledChange: m_label->setEnabled(m_box->isEnabled()); break;
                    default: break;
                }
            }
            return false;
        }

    private:
        QPointer<QCheckBox> m_box;
        QPointer<QWidget> m_row;
        QPointer<QLabel> m_label;
    };

    QWidget *makeToggleRow(QWidget *parent, QCheckBox *box) {
        auto *row = new QWidget(parent);
        row->setObjectName(box->objectName() + QStringLiteral("_row"));
        row->setMinimumWidth(180);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);

        auto *label = new QLabel(box->text(), row);
        label->setProperty("slRole", "toggleLabel");
        label->setToolTip(box->toolTip());
        label->setCursor(Qt::PointingHandCursor);
        label->ensurePolished();
        label->setMinimumWidth(label->sizeHint().width()); // never clip the mode names

        box->setText(QString());
        box->setProperty("slRole", "toggle");
        box->setCursor(Qt::PointingHandCursor);

        layout->addWidget(label);
        layout->addStretch(1);
        layout->addWidget(box);
        new ToggleLink(box, row, label);
        repolish(box);
        return row;
    }

    // ----------------------------------------------------------------- cards

    QFrame *makeCard(QWidget *parent, const char *name) {
        auto *card = new QFrame(parent);
        card->setObjectName(QLatin1String(name));
        card->setFrameShape(QFrame::NoFrame);
        return card;
    }

    // Icon-only width of a pill: 8px padding, 24px icon, 12px padding.
    constexpr int kCompactPillWidth = 44;

    void stylePill(QToolButton *button, const char *iconName) {
        button->setStyleSheet(QString()); // drops the .ui's per-button sheet
        button->setProperty("slRole", "pill");
        button->setIcon(icon(iconName));
        button->setIconSize(QSize(24, 24));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setCursor(Qt::PointingHandCursor);
        // May be squeezed to its icon; CompactPills then drops the label.
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        button->setMinimumWidth(kCompactPillWidth);
        if (button->toolTip().isEmpty()) button->setToolTip(button->text());
        repolish(button);
    }

    // The labelled pills need about 1150px. Below the width they need, they show only
    // their icons (the label stays in the tooltip), so the window can still be made small.
    class CompactPills final : public QObject {
    public:
        CompactPills(QWidget *central, const QList<QToolButton *> &pills, int fixedWidth)
            : QObject(central), m_central(central), m_fixedWidth(fixedWidth) {
            for (QToolButton *pill : pills) m_pills << pill;
            central->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (watched == m_central.data() && event->type() == QEvent::Resize) relayout();
            return false;
        }

    private:
        // As the sheet lays a labelled pill out: padding, icon, icon-text gap, text, padding.
        static int labelledWidth(const QToolButton *pill) {
            return 8 + pill->iconSize().width() + 6 + pill->fontMetrics().horizontalAdvance(pill->text()) + 12 + 4;
        }

        void relayout() {
            if (m_central.isNull()) return;
            int needed = m_fixedWidth;
            for (const auto &pill : m_pills) {
                if (!pill.isNull() && !pill->isHidden()) needed += labelledWidth(pill.data());
            }
            const auto style = m_central->width() < needed ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon;
            for (const auto &pill : m_pills) {
                if (!pill.isNull() && pill->toolButtonStyle() != style) pill->setToolButtonStyle(style);
            }
        }

        QPointer<QWidget> m_central;
        QList<QPointer<QToolButton>> m_pills;
        int m_fixedWidth;
    };

    QToolButton *makePill(QWidget *parent, const char *name, const QString &text, const char *iconName) {
        auto *button = new QToolButton(parent);
        button->setObjectName(QLatin1String(name));
        button->setText(text);
        stylePill(button, iconName);
        return button;
    }

    // A pill that fires an existing menu action, so it shares that action's handler and enabled state.
    QToolButton *makeActionPill(QWidget *parent, const char *name, const QString &text, const char *iconName,
                                QAction *action) {
        auto *button = makePill(parent, name, text, iconName);
        button->setToolTip(action->text());
        button->setEnabled(action->isEnabled());
        QObject::connect(button, &QToolButton::clicked, action, &QAction::trigger);
        QObject::connect(action, &QAction::enabledChanged, button, &QWidget::setEnabled);
        return button;
    }

    // ----------------------------------------------------------------- font

    // The family PT Root UI registered under, e.g. "PT Root UI VF"; empty until loaded.
    QString &themeFontFamily() {
        static QString family;
        return family;
    }

    // Puts PT Root UI first in the application font's families while the theme is on, and
    // takes it off again otherwise. setFamilies keeps the old list as the fallback.
    void syncThemeFont() {
        const QString family = themeFontFamily();
        if (family.isEmpty()) return;

        QFont font = QApplication::font();
        QStringList families = font.families();
        if (families.isEmpty()) families << font.family();
        const bool active = Shadowlos::Chrome::ThemeActive();
        const bool present = families.first() == family;
        if (active == present) return;

        if (active) {
            // A family chosen in Basic Settings outranks the theme's.
            if (Configs::dataManager != nullptr && Configs::dataManager->settingsRepo &&
                !Configs::dataManager->settingsRepo->font.isEmpty()) {
                return;
            }
            families.prepend(family);
        } else {
            families.removeAll(family);
        }
        font.setFamilies(families);
        QApplication::setFont(font);
    }

} // namespace Shadowlos::ChromeImpl

namespace Shadowlos::Chrome {

    bool IsThemeName(const QString &theme) {
        return theme.trimmed().compare(QLatin1String(ThemeName), Qt::CaseInsensitive) == 0;
    }

    bool ThemeActive() {
        return IsThemeName(themeManager()->current_theme);
    }

    void ApplyThemeFont(bool active) {
        static bool attempted = false;
        if (active && !attempted) {
            attempted = true;
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/shadowlos/fonts/PTRootUI-VF.ttf"));
            if (id >= 0) ChromeImpl::themeFontFamily() = QFontDatabase::applicationFontFamilies(id).value(0);
            if (ChromeImpl::themeFontFamily().isEmpty()) qWarning() << "[Shadowlos] could not load PT Root UI";
        }
        // Deferred, and it reads the installed theme when it runs. The first call comes from
        // MainWindow's constructor before setupUi(): an app font change there would reach
        // MainWindow::changeEvent, which touches ui widgets that do not exist yet.
        static bool pending = false;
        if (pending) return;
        pending = true;
        QTimer::singleShot(0, qApp, [] {
            pending = false;
            ChromeImpl::syncThemeFont();
        });
    }

    QString StatusLine(const QString &text) {
        QString line = text;
        line.replace(QLatin1Char('\n'), QStringLiteral("  |  "));
        return line;
    }

    // ----------------------------------------------------------- simple view

    // "Simple view" (the default) shows only what a user picks a server by: name, test
    // result and traffic, without the logs/connections panel. The full view is upstream's.
    bool simpleViewOn() {
        return Configs::dataManager->settingsRepo->shadowlos_simple_view != 0;
    }

    // Upstream keeps the columns Interactive and re-applies the widths it saved for the group
    // (any resize is saved), so the widths are set here on every refit and that is what gets
    // saved and re-applied. Test result and traffic fit their contents. In the full view the
    // name column takes the rest of the row. In the simple view the name column fits the
    // names too, so the three columns sit together on the left, and traffic, the last one,
    // takes what is left. In the full view, a column that was hidden when the widths were
    // saved comes back at a usable width.
    void fitColumns(Ui::MainWindow *ui) {
        auto *table = ui->profilesTableView;
        auto *header = table->horizontalHeader();
        auto *view = static_cast<QAbstractItemView *>(table);
        const bool simple = simpleViewOn();
        const int padding = simple ? 48 : 0;
        const auto fit = [&](int col) {
            return std::max(header->sectionSizeHint(col), view->sizeHintForColumn(col)) + padding;
        };
        const auto setWidth = [&](int col, int width) {
            if (header->sectionSize(col) == width) return;
            header->setSectionResizeMode(col, QHeaderView::Interactive);
            header->resizeSection(col, width);
        };
        if (!simple) {
            for (int col : {ProfilesTableModel::ColType, ProfilesTableModel::ColAddress})
                if (header->sectionSize(col) < 40) header->resizeSection(col, 140);
        }
        const int viewport = table->viewport()->width();
        const int test = fit(ProfilesTableModel::ColTestResult);
        int traffic = fit(ProfilesTableModel::ColTraffic);
        int others = 0;
        for (int col = 0; col < header->count(); ++col)
            if (col != ProfilesTableModel::ColName && col != ProfilesTableModel::ColTestResult &&
                col != ProfilesTableModel::ColTraffic && !header->isSectionHidden(col))
                others += header->sectionSize(col);
        int name;
        if (simple) {
            name = std::clamp(fit(ProfilesTableModel::ColName), 240, std::max(240, viewport - test - traffic));
            traffic = std::max(traffic, viewport - name - test);
        } else {
            name = std::max(160, viewport - others - test - traffic);
        }
        setWidth(ProfilesTableModel::ColTestResult, test);
        setWidth(ProfilesTableModel::ColTraffic, traffic);
        setWidth(ProfilesTableModel::ColName, name);
    }

    void applySimpleView(Ui::MainWindow *ui) {
        using namespace Shadowlos::ChromeImpl;
        const bool simple = simpleViewOn();
        auto *table = ui->profilesTableView;
        for (int col : {ProfilesTableModel::ColType, ProfilesTableModel::ColAddress})
            table->setColumnHidden(col, simple);
        table->verticalHeader()->setVisible(!simple);
        // The sheet sets a bigger font and a left indent on the rows in the simple view.
        table->setProperty("slSimple", simple);
        repolish(table);
        repolish(table->horizontalHeader());
        // Column titles over their values: the simple view's last column is wide.
        table->horizontalHeader()->setDefaultAlignment(simple ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
        table->verticalHeader()->setDefaultSectionSize(simple ? 64 : 44);
        if (QWidget *panel = ui->splitter->widget(1)) panel->setVisible(!simple);
        fitColumns(ui);
    }

    // Refits after the table resizes or upstream re-lays its columns out (it does that on a
    // zero timer after each refresh, so ours runs a little later).
    class ColumnFitter final : public QObject {
    public:
        explicit ColumnFitter(Ui::MainWindow *ui) : QObject(ui->profilesTableView), m_ui(ui) {
            ui->profilesTableView->viewport()->installEventFilter(this);
            m_timer.setSingleShot(true);
            m_timer.setInterval(50);
            QObject::connect(&m_timer, &QTimer::timeout, this, [this] { fitColumns(m_ui); });
            auto *model = ui->profilesTableView->model();
            if (model == nullptr) return;
            QObject::connect(model, &QAbstractItemModel::modelReset, this, [this] { m_timer.start(); });
            QObject::connect(model, &QAbstractItemModel::layoutChanged, this, [this] { m_timer.start(); });
            QObject::connect(model, &QAbstractItemModel::rowsInserted, this, [this] { m_timer.start(); });
        }

    protected:
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Resize) m_timer.start();
            return false;
        }

    private:
        Ui::MainWindow *m_ui;
        QTimer m_timer;
    };

    // The simple / full view switch sits in the tab corner, left of the filter button.
    void installSimpleView(Ui::MainWindow *ui) {
        using namespace Shadowlos::ChromeImpl;
        auto *filter = ui->tabWidget->cornerWidget(Qt::TopRightCorner);
        auto *corner = new QWidget(ui->tabWidget);
        auto *layout = new QHBoxLayout(corner);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        auto *box = new QCheckBox(translate("Simple view"), corner);
        box->setChecked(simpleViewOn());
        auto *row = makeToggleRow(corner, box);
        row->setMinimumWidth(0);
        layout->addWidget(row);
        // setCornerWidget hides the widget it replaces, so re-show the filter in the new corner.
        ui->tabWidget->setCornerWidget(corner, Qt::TopRightCorner);
        if (filter != nullptr) {
            layout->addWidget(filter);
            filter->show();
        }
        corner->show();

        QObject::connect(box, &QCheckBox::toggled, box, [ui](bool checked) {
            auto *settings = Configs::dataManager->settingsRepo.get();
            settings->shadowlos_simple_view = checked ? 1 : 0;
            settings->Save();
            applySimpleView(ui);
        });
        new ColumnFitter(ui);
        applySimpleView(ui);
    }

    // Measures latency once at startup, as the "Measure latency" pill would, as soon as the
    // core is up and the current group has servers (on a first run the subscription has to
    // arrive first). Gives up after two minutes.
    void installStartupLatencyTest(QMainWindow *window, Ui::MainWindow *ui) {
        auto *timer = new QTimer(window);
        timer->setInterval(1000);
        auto tries = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, window, [timer, ui, tries] {
            if (++*tries > 120) {
                timer->deleteLater();
                return;
            }
            if (!Configs::dataManager->settingsRepo->core_running) return;
            auto group = Configs::dataManager->groupsRepo->CurrentGroup();
            if (group == nullptr || group->Profiles().isEmpty()) return;
            if (!ui->actionUrl_Test_Group->isEnabled()) return;
            timer->stop();
            timer->deleteLater();
            // A moment for the core to finish starting before it is asked to dial out.
            QTimer::singleShot(1500, ui->actionUrl_Test_Group, &QAction::trigger);
        });
        timer->start();
    }

    void Install(QMainWindow *window, Ui::MainWindow *ui) {
        using namespace Shadowlos::ChromeImpl;

        static bool appFilterInstalled = false;
        if (!appFilterInstalled) {
            appFilterInstalled = true;
            qApp->installEventFilter(new AppFilter(qApp));
        }

        auto *central = new QWidget(window);
        central->setObjectName(QStringLiteral("slCentral"));
        auto *root = new QVBoxLayout(central);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(12);

        // Menu card: Program / Settings / Groups / Routing / Tools, text and a chevron.
        auto *menuCard = makeCard(central, "slMenuCard");
        menuCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *menuRow = new QHBoxLayout(menuCard);
        menuRow->setContentsMargins(8, 0, 8, 0);
        menuRow->setSpacing(8);
        for (QToolButton *button : {ui->toolButton_program, ui->toolButton_preferences, ui->toolButton_testing,
                                    ui->toolButton_routing, ui->toolButton_tools}) {
            ui->horizontalLayout_2->removeWidget(button);
            button->setStyleSheet(QString());
            button->setIcon(QIcon());
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setProperty("slRole", "menu");
            button->setCursor(Qt::PointingHandCursor);
            repolish(button);
            menuRow->addWidget(button);
        }
        menuRow->addStretch(1);
        root->addWidget(menuCard);
        // Read by MainWindow::applyTopBarMetrics: the design sizes these to their labels.
        window->setProperty("slVariableWidthMenus", true);

        auto *actionsRow = new QHBoxLayout();
        actionsRow->setSpacing(8);

        // Power card. The new button replaces uic's StartStopButton before anything connects
        // to it; MainWindow only ever reaches it through ui->toolButton_startstop.
        auto *powerCard = makeCard(central, "slCard");
        powerCard->setFixedWidth(96);
        powerCard->setMinimumHeight(96);
        auto *powerLayout = new QHBoxLayout(powerCard);
        powerLayout->setContentsMargins(16, 16, 16, 16);
        auto *power = new PowerButton(powerCard);
        power->setObjectName(QStringLiteral("toolButton_startstop"));
        powerLayout->addWidget(power, 0, Qt::AlignCenter);
        if (StartStopButton *old = ui->toolButton_startstop) {
            ui->horizontalLayout_2->removeWidget(old);
            delete old;
        }
        ui->toolButton_startstop = power;
        actionsRow->addWidget(powerCard);

        // Actions card.
        auto *actionsCard = makeCard(central, "slCard");
        auto *actionsLayout = new QHBoxLayout(actionsCard);
        actionsLayout->setContentsMargins(16, 8, 16, 8);
        actionsLayout->setSpacing(8);

        auto *add = makePill(actionsCard, "slAddConfig", translate("Add config"), "add-config");
        add->setPopupMode(QToolButton::InstantPopup);
        auto *addMenu = new QMenu(add);
        addMenu->addAction(ui->menu_add_from_clipboard);
        addMenu->addAction(ui->menu_add_from_input);
        addMenu->addAction(ui->actionAdd_profile_from_File);
        addMenu->addAction(ui->menu_scan_qr);
        add->setMenu(addMenu);
        actionsLayout->addWidget(add);

        ui->horizontalLayout_2->removeWidget(ui->toolButton_update_subs);
        stylePill(ui->toolButton_update_subs, "refresh");
        actionsLayout->addWidget(ui->toolButton_update_subs);

        auto *speedtest = makeActionPill(actionsCard, "slSpeedtest", translate("Measure speed"), "chart",
                                         ui->actionSpeedtest_Group);
        actionsLayout->addWidget(speedtest);
        auto *latency = makeActionPill(actionsCard, "slLatency", translate("Measure latency"), "latency",
                                       ui->actionUrl_Test_Group);
        actionsLayout->addWidget(latency);
        actionsRow->addWidget(actionsCard);

        // Debug card.
        auto *debugCard = makeCard(central, "slCard");
        auto *debugLayout = new QHBoxLayout(debugCard);
        debugLayout->setContentsMargins(16, 8, 16, 8);
        ui->horizontalLayout_2->removeWidget(ui->toolButton_debug);
        stylePill(ui->toolButton_debug, "debug");
        debugLayout->addWidget(ui->toolButton_debug);
        actionsRow->addWidget(debugCard);

        // Modes card: just the switches, sized to them. The status panel (speed tests,
        // restart notices) takes the rest of the row, outside any card.
        auto *modesCard = makeCard(central, "slCard");
        auto *modesLayout = new QHBoxLayout(modesCard);
        modesLayout->setContentsMargins(16, 8, 16, 8);
        modesLayout->setSpacing(16);
        auto *toggles = new QVBoxLayout();
        toggles->setSpacing(12);
        toggles->addStretch(1);
        for (QCheckBox *box : {ui->checkBox_VPN, ui->checkBox_SystemProxy, ui->system_dns}) {
            ui->verticalLayout_4->removeWidget(box);
            toggles->addWidget(makeToggleRow(modesCard, box));
        }
        toggles->addStretch(1);
        modesLayout->addLayout(toggles);
        actionsRow->addWidget(modesCard);
        ui->horizontalLayout_2->removeWidget(ui->data_view);
        ui->data_view->setParent(central);
        actionsRow->addWidget(ui->data_view, 1);

        root->addLayout(actionsRow);

        // Everything on the row except the pills' labels: root margins, power card, row gaps,
        // card margins, gaps between pills, and the modes card's switches (sized for the
        // long Russian label "Только браузеры (системный прокси)").
        constexpr int kRowFixedWidth = 24 + 96 + 3 * 8 + 32 + 3 * 8 + 32 + (300 + 32);
        new CompactPills(central,
                         {add, ui->toolButton_update_subs, speedtest, latency, ui->toolButton_debug},
                         kRowFixedWidth);

        // Profile tabs over the Logs / Connections / Graph tabs, as before.
        ui->verticalLayout_3->removeWidget(ui->splitter);
        root->addWidget(ui->splitter, 1);

        // Status card.
        root->addSpacing(4);
        auto *statusCard = makeCard(central, "slStatusCard");
        auto *statusLayout = new QHBoxLayout(statusCard);
        statusLayout->setContentsMargins(16, 8, 16, 8);
        statusLayout->setSpacing(16);
        for (QLabel *label : {ui->label_running, ui->label_inbound, ui->label_speed}) {
            ui->horizontalLayout->removeWidget(label);
            label->setProperty("slRole", "status");
            label->setWordWrap(false);
            label->setMinimumWidth(1); // long server names clip instead of widening the window
            repolish(label);
        }
        statusLayout->addWidget(ui->label_running);
        statusLayout->addStretch(1);
        statusLayout->addWidget(ui->label_inbound);
        statusLayout->addStretch(1);
        statusLayout->addWidget(ui->label_speed);
        root->addWidget(statusCard);

        // Every widget uic made has been moved into `central`; this deletes the empty old
        // container and its layouts, so drop the pointers to them.
        window->setCentralWidget(central);
        ui->centralwidget = central;
        ui->verticalLayout_3 = nullptr;
        ui->horizontalLayout_2 = nullptr;
        ui->verticalLayout_4 = nullptr;
        ui->horizontalLayout = nullptr;
        ui->verticalSpacer_modes_top = nullptr;
        ui->verticalSpacer_modes_bottom = nullptr;

        auto *table = ui->profilesTableView;
        table->setItemDelegate(new RowDelegate(table));
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);

        QObject::connect(themeManager(), &ThemeManager::themeChanged, window, [] { applyCaptionToVisibleWindows(); });

        // After MainWindow's constructor, which sets these up later than this runs.
        QTimer::singleShot(0, window, [window, ui] {
            // MainWindow sizes the start button to the menu buttons' height; the card wants 64.
            ui->toolButton_program->removeEventFilter(window);
            ui->toolButton_startstop->setFixedSize(64, 64);

            if (auto *filter = qobject_cast<QToolButton *>(ui->tabWidget->cornerWidget(Qt::TopRightCorner))) {
                filter->setObjectName(QStringLiteral("slFilterButton"));
                filter->setIcon(icon("filter"));
                filter->setIconSize(QSize(16, 16));
                filter->setFixedSize(28, 28);
                filter->setCursor(Qt::PointingHandCursor);
                repolish(filter);
            }

            installSimpleView(ui);
            installStartupLatencyTest(window, ui);

            applyCaptionToVisibleWindows();
        });
    }

} // namespace Shadowlos::Chrome
