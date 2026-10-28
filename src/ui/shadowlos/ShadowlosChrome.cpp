#include "include/ui/shadowlos/ShadowlosChrome.hpp"
#include "include/ui/shadowlos/ShadowlosUpdater.hpp"
#include "include/ui/shadowlos/ShadowlosJournal.hpp"
#include "include/ui/shadowlos/ShadowlosRouting.hpp"
#include "include/global/ShadowlosBootstrap.hpp"

// Brings in ui_mainwindow.h, i.e. the full Ui::MainWindow. Only when MW_INTERFACE is unset,
// which is why CMake keeps this file out of unity batches.
#include "include/ui/mainwindow.h"
#include "NkrVersion.h"

#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/global/Utils.hpp"
#include "include/database/SettingsRepo.h"
#include "include/ui/setting/ThemeManager.hpp"
#include "include/ui/utils/DataViewHtmlGenerator.h"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/widget/StartStopButton.hpp"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QConicalGradient>
#include <QLinearGradient>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QScreen>
#include <QScrollBar>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <QToolButton>
#include <QToolTip>
#include <QWidgetAction>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QEasingCurve>
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

    // Dev screenshots: SHADOWLOS_UI_DEMO is a comma list (on|busy|off, minimal|normal|advanced,
    // min|max, menu|tip). Always false in prod, where UiDemo() is empty.
    bool demoHas(const char *token) {
        const QString demo = Shadowlos::Chrome::UiDemo();
        return !demo.isEmpty() && demo.split(QLatin1Char(',')).contains(QLatin1String(token));
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

    // The design's connect button: a ring and a disc, grey while off, blue while connecting
    // and green while connected, cross-fading between them. Takes over StartStopButton's painting
    // only; state, tooltips and animations stay the base class's.
    class PowerButton final : public StartStopButton {
    public:
        explicit PowerButton(QWidget *parent) : StartStopButton(parent) {
            setAttribute(Qt::WA_Hover, true);
            setFixedSize(64, 64);
            m_tintAnim = new QVariantAnimation(this);
            m_tintAnim->setDuration(500);
            m_tintAnim->setEasingCurve(QEasingCurve::InOutSine);
            QObject::connect(m_tintAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
                m_tint = v.toReal();
                update();
            });
        }

        QSize sizeHint() const override { return size(); }

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

            State st = state();
            if (demoHas("on")) st = State::Running;
            else if (demoHas("busy")) st = State::Connecting;
            else if (demoHas("off")) st = State::Idle;
            const bool running = st == State::Running;
            const bool busy = st == State::Connecting || st == State::Disconnecting;
            const bool disabled = st == State::Disabled;
            const bool hovered = underMouse() && isEnabled();

            // As the design: grey while off (ready to connect), blue while connecting or
            // disconnecting (with the spinner), green only while connected. The colours
            // cross-fade from wherever the last fade got to.
            const Tone target = running ? Tone::Green : busy ? Tone::Blue : Tone::Grey;
            if (target != m_toneTarget || m_first) {
                m_from = m_first ? tone(target) : current();
                m_to = tone(target);
                m_toneTarget = target;
                m_tintAnim->stop();
                m_tint = m_first ? 1.0 : 0.0;
                if (!m_first) {
                    // Into blue quickly, so a press shows at once; out of it more softly.
                    m_tintAnim->setDuration(target == Tone::Blue ? 120 : 250);
                    m_tintAnim->setStartValue(0.0);
                    m_tintAnim->setEndValue(1.0);
                    m_tintAnim->start();
                }
                m_first = false;
            }
            const Palette now = current();
            const auto pick = [&now, hovered](int i) { return i == 2 && hovered ? now.c[5] : now.c[i]; };

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
            QColor outline;
            if (disabled) {
                ring.setColorAt(0, hex(0x111517));
                ring.setColorAt(1, hex(0x1A2023));
                outline = hex(0x2E3438);
            } else {
                ring.setColorAt(0, pick(0));
                ring.setColorAt(1, pick(1));
                outline = pick(2);
            }
            p.setPen(QPen(outline, 1.33));
            p.setBrush(ring);
            p.drawEllipse(outer.adjusted(0.67, 0.67, -0.67, -0.67));

            // Disc: 57px in the design.
            const qreal inset = side * 3.5 / 64.0;
            const QRectF inner = outer.adjusted(inset, inset, -inset, -inset);
            QLinearGradient disc(inner.topLeft(), inner.bottomLeft());
            disc.setColorAt(0, disabled ? hex(0x0B0D0F) : pick(3));
            disc.setColorAt(1, disabled ? hex(0x111517) : pick(4));
            p.setPen(Qt::NoPen);
            p.setBrush(disc);
            p.drawEllipse(inner);

            if (busy) {
                // A comet: a bright head running clockwise and a tail that fades out behind it.
                constexpr qreal kSpan = 270.0;
                const qreal head = -spin() - kSpan; // the arc's clockwise end leads
                QConicalGradient comet(c, head);
                QColor tail(Qt::white);
                tail.setAlphaF(0.0);
                // Alpha falls off along an ease-out curve, so the tail has no visible edge.
                for (int i = 0; i <= 8; ++i) {
                    const qreal k = i / 8.0;
                    QColor stop(Qt::white);
                    stop.setAlphaF((1.0 - k) * (1.0 - k));
                    comet.setColorAt(k * kSpan / 360.0, stop);
                }
                comet.setColorAt(0.97, tail);
                comet.setColorAt(1.0, Qt::white); // the head's round cap
                QPen arc(QBrush(comet), qMax(2.0, side * 0.045));
                arc.setCapStyle(Qt::RoundCap);
                p.setPen(arc);
                p.setBrush(Qt::NoBrush);
                const qreal a = arc.widthF() / 2 + 0.5;
                p.drawArc(outer.adjusted(a, a, -a, -a), static_cast<int>(-spin() * 16),
                          static_cast<int>(-kSpan * 16));
            }

            const QColor glyph = disabled ? hex(0x474D53) : QColor(Qt::white);

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

    private:
        enum class Tone { Grey, Blue, Green };
        // Ring top, ring bottom, outline, disc top, disc bottom, outline under the mouse.
        struct Palette {
            QColor c[6];
        };

        static Palette tone(Tone t) {
            switch (t) {
                case Tone::Green: return {{hex(0x2E6B1F), hex(0x1F4A16), hex(0x86D75E), hex(0x86D75E), hex(0x5DB83A), hex(0xA6E88A)}};
                case Tone::Blue: return {{hex(0x0072CC), hex(0x1A374D), hex(0x33A5FF), hex(0x33A5FF), hex(0x008FFF), hex(0x66BBFF)}};
                default: return {{hex(0x3A4248), hex(0x23292D), hex(0x5E676E), hex(0x5E676E), hex(0x474D53), hex(0x8A949B)}};
            }
        }

        Palette current() const {
            Palette p;
            const qreal t = m_tint;
            for (int i = 0; i < 6; ++i) {
                const QColor &a = m_from.c[i], &b = m_to.c[i];
                p.c[i] = QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                                          a.blueF() + (b.blueF() - a.blueF()) * t);
            }
            return p;
        }

        QVariantAnimation *m_tintAnim = nullptr;
        qreal m_tint = 1.0;
        bool m_first = true;
        Tone m_toneTarget = Tone::Grey;
        Palette m_from = tone(Tone::Grey);
        Palette m_to = tone(Tone::Grey);
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
    constexpr int kDenseFontPx = 12; // Продвинутый: stock Throne's dense rows
    constexpr int kDenseRowPx = 28;

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
            if (parent() != nullptr) option->font.setPixelSize(parent()->property("slSimple").toBool() ? kSimpleFontPx : kDenseFontPx);
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

    // A two-line toggle name: the sub-line in rich text, so its size is set here (bigger when wide).
    void setToggleText(QLabel *label, bool wide) {
        const QString sub = label->property("slSub").toString();
        if (sub.isEmpty()) return;
        label->setText(QStringLiteral("%1<br><span style=\"color:#8A949B; font-size:%3px; font-weight:400\">%2</span>")
                           .arg(label->property("slMain").toString().toHtmlEscaped(), sub.toHtmlEscaped())
                           .arg(wide ? 13 : 11));
    }

    QWidget *makeToggleRow(QWidget *parent, QCheckBox *box) {
        auto *row = new QWidget(parent);
        row->setObjectName(box->objectName() + QStringLiteral("_row"));
        row->setMinimumWidth(180);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);

        // A long name puts its bracketed part on a second, smaller grey line:
        // "Только браузеры" over "(системный прокси)", so the card stays narrow.
        const QString text = box->text();
        const int bracket = text.indexOf(QStringLiteral(" ("));
        auto *label = new QLabel(row);
        if (bracket > 0 && text.endsWith(QLatin1Char(')'))) {
            label->setTextFormat(Qt::RichText);
            label->setProperty("slMain", text.left(bracket));
            label->setProperty("slSub", text.mid(bracket + 1));
            setToggleText(label, false);
        } else {
            label->setText(text);
        }
        label->setProperty("slRole", "toggleLabel");
        label->setToolTip(box->toolTip());
        label->setCursor(Qt::PointingHandCursor);
        // Never clip the mode names: the layout asks the label for its width each time, so
        // it follows the font the sheet and the system settle on (a width fixed here goes
        // stale when that font is wider, and the switch then covers the text).
        label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

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
    // The quick actions' fixed height, labelled or not, in any window.
    constexpr int kPillHeight = 44;
    // The labelled quick actions' text in a wide window (shadowlos.qss, [slWide="true"]).
    constexpr int kWidePillFontPx = 15;
    // The top row's one height (more only if a card's content needs it, e.g. a fourth toggle).
    constexpr int kTopRowHeight = 152;
    // The power card: the 64px button (96px in Минимальный) and its margins.
    constexpr int kPowerCardWidth = 112;
    constexpr int kPowerCardWidthMini = 144;

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

    QToolButton *makePill(QWidget *parent, const char *name, const QString &text, const char *iconName) {
        auto *button = new QToolButton(parent);
        button->setObjectName(QLatin1String(name));
        button->setText(text);
        stylePill(button, iconName);
        return button;
    }

    // The quick actions' hover text: the name and one line on what it does.
    void setPillTip(QToolButton *button, const QString &name, const QString &line) {
        button->setToolTip(QStringLiteral("<b>%1</b><br>%2").arg(name.toHtmlEscaped(), line.toHtmlEscaped()));
        button->setAccessibleName(name);
    }

    // A pill that fires an existing menu action, so it shares that action's handler and enabled state.
    QToolButton *makeActionPill(QWidget *parent, const char *name, const QString &text, const char *iconName,
                                QAction *action) {
        auto *button = makePill(parent, name, text, iconName);
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

    // ------------------------------------------------------- corner labels
    // The version (bottom bar, left) and, at the status card title's right end, how long the
    // running profile has been connected. One 1 s timer, running only while connected.
    struct Corner {
        QPointer<QLabel> version;
        QPointer<QLabel> stopwatch;
        QPointer<QTimer> timer;
        QElapsedTimer since;
    };

    Corner &corner() {
        static Corner c;
        return c;
    }

    // True while the main window is hidden (tray) or minimized: nothing needs repainting then.
    // Unset before Install (the self-test's throwaway card), so it never gates that.
    std::function<bool()> &uiHidden() {
        static std::function<bool()> hidden;
        return hidden;
    }

    bool hiddenNow() {
        return uiHidden() && uiHidden()();
    }

    void tickStopwatch() {
        Corner &c = corner();
        if (c.stopwatch == nullptr || !c.since.isValid() || hiddenNow()) return;
        c.stopwatch->setText(Shadowlos::Chrome::StopwatchText(c.since.elapsed() / 1000));
    }

    QLabel *makeCornerLabel(QWidget *parent, const QString &name) {
        auto *label = new QLabel(parent);
        label->setObjectName(name);
        label->setProperty("slRole", "corner");
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        return label;
    }

    // The two labels and the timer, owned by `owner` (the status card).
    Corner &makeCorners(QWidget *owner) {
        Corner &c = corner();
        c.since.invalidate();
        c.version = makeCornerLabel(owner, QStringLiteral("slCornerVersion"));
        c.version->setText(QStringLiteral(NKR_VERSION));
        c.stopwatch = makeCornerLabel(owner, QStringLiteral("slCornerStopwatch"));
        c.stopwatch->setVisible(false);
        c.timer = new QTimer(owner);
        c.timer->setInterval(1000);
        QObject::connect(c.timer, &QTimer::timeout, owner, [] { tickStopwatch(); });
        return c;
    }

    // ---------------------------------------------------------- status card

    // A one-line label that elides instead of widening the window. Its minimum width is that of
    // floorText, the longest usual sentence, so those are never cut off; only longer ones
    // (server names) elide.
    class ElideLabel final : public QLabel {
    public:
        ElideLabel(QWidget *parent, const char *role, const QString &floorText = {})
            : QLabel(parent), m_floor(floorText) {
            setProperty("slRole", role);
            setTextFormat(Qt::PlainText);
            // Preferred, not Ignored: an ignored policy also drops minimumSizeHint().
            setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        }

        void setFullText(const QString &text) {
            if (text == m_full) return;
            m_full = text;
            setToolTip(fontMetrics().horizontalAdvance(text) > width() ? text : QString());
            updateGeometry();
            elide();
        }

        QSize sizeHint() const override {
            ensurePolished();
            return {fontMetrics().horizontalAdvance(m_full) + 4, fontMetrics().height() + 2};
        }

        QSize minimumSizeHint() const override {
            ensurePolished();
            // The floor alone, not the text shown: the window's minimum is set once per view,
            // so it must not depend on what the card happens to say.
            const int floor = m_floor.isEmpty() ? 0 : fontMetrics().horizontalAdvance(m_floor) + 4;
            return {floor, fontMetrics().height() + 2};
        }

    protected:
        void resizeEvent(QResizeEvent *event) override {
            QLabel::resizeEvent(event);
            elide();
        }

        void changeEvent(QEvent *event) override {
            QLabel::changeEvent(event);
            if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
                updateGeometry();
                elide();
            }
        }

    private:
        // A line of « · » parts loses whole parts from its end first; only a first part that
        // does not fit alone is cut with «…».
        void elide() {
            const int room = qMax(0, width());
            static const QString sep = QStringLiteral(" · ");
            QStringList parts = m_full.split(sep);
            while (parts.size() > 1 && fontMetrics().horizontalAdvance(parts.join(sep)) > room) parts.removeLast();
            const QString shown = fontMetrics().elidedText(parts.join(sep), Qt::ElideRight, room);
            if (shown != text()) QLabel::setText(shown);
            setToolTip(shown != m_full ? m_full : QString());
        }

        QString m_full;
        QString m_floor;
    };

    // A traffic amount as people say it: «619 МБ», «1.2 ГБ».
    QString shortSize(qint64 bytes) {
        constexpr qint64 k = 1024;
        if (bytes < k) return translate("%1 B").arg(bytes);
        // A decimal comma where the units are Russian («1,2 ГБ»).
        const bool comma = translate("%1 GB") != QLatin1String("%1 GB");
        const auto amount = [comma](double v) {
            QString n = v < 10 ? QString::number(v, 'f', 1) : QString::number(qRound64(v));
            if (comma) n.replace(QLatin1Char('.'), QLatin1Char(','));
            return n;
        };
        if (bytes < k * k) return translate("%1 KB").arg(amount(double(bytes) / k));
        if (bytes < k * k * k) return translate("%1 MB").arg(amount(double(bytes) / (k * k)));
        return translate("%1 GB").arg(amount(double(bytes) / (k * k * k)));
    }

    // What the data view shows, in plain words: a busy panel (a test or a download) with its
    // progress, the restart notice, and a note (VPN endpoint, auto selector).
    struct DataPanel {
        bool busy = false;
        QString title;
        QString detail;
        qint64 done = 0;
        qint64 total = 0; // 0: no bar, -1: a bar without an end
        bool restart = false;
        QStringList reasons;
        QString note;
        bool noteProblem = false;
    };

    QString ofText(qint64 done, qint64 total) {
        return translate("%1 of %2").arg(done).arg(total);
    }

    DataViewHtmlGenerator *&generator() {
        static DataViewHtmlGenerator *g = nullptr;
        return g;
    }

} // namespace Shadowlos::ChromeImpl

// The friend DataViewHtmlGenerator names: reads its panels under its own lock.
struct ShadowlosDataViewPeek {
    static Shadowlos::ChromeImpl::DataPanel read(DataViewHtmlGenerator &g) {
        using namespace Shadowlos::ChromeImpl;
        DataPanel p;
        QMutexLocker lock(&g.mu_);
        const qint64 progress = g.testProgress.load();
        if (g.speedtest_.visible) {
            const auto &t = g.speedtest_;
            p.busy = true;
            QStringList parts;
            if (t.totalProfiles > 1) parts << ofText(progress, t.totalProfiles);
            if (t.kind == DataViewHtmlGenerator::SpeedtestPanelState::Kind::Country) {
                p.title = translate("Finding server countries");
            } else {
                p.title = translate("Measuring speed");
                if (!t.profileName.isEmpty()) parts << t.profileName;
                if (!t.dlSpeed.isEmpty()) parts << QStringLiteral("↓ %1  ↑ %2").arg(t.dlSpeed, t.ulSpeed);
            }
            p.detail = parts.join(QStringLiteral(" · "));
            p.done = progress;
            p.total = t.totalProfiles > 1 ? t.totalProfiles : -1;
        } else if (g.latencyTest_.visible) {
            const auto &t = g.latencyTest_;
            p.busy = true;
            p.title = translate("Checking servers");
            const bool url = t.kind == DataViewHtmlGenerator::LatencyTestPanelState::Kind::Url;
            p.detail = t.totalProfiles > 1
                           ? (url ? translate("Measuring latency: %1") : translate("Checking addresses: %1"))
                                 .arg(ofText(progress, t.totalProfiles))
                           : (url ? translate("Measuring latency") : translate("Checking addresses"));
            p.done = progress;
            p.total = t.totalProfiles > 1 ? t.totalProfiles : -1;
        } else if (g.download_.visible) {
            const auto &r = g.download_.report;
            p.busy = true;
            p.title = translate("Downloading");
            p.detail = r.totalSize > 0 ? QStringLiteral("%1 · %2 / %3").arg(r.fileName, ReadableSize(r.downloadedSize), ReadableSize(r.totalSize))
                                       : r.fileName;
            p.done = r.downloadedSize;
            p.total = r.totalSize > 0 ? r.totalSize : -1;
        }
        if (g.pendingRestart_.visible) {
            p.restart = true;
            p.reasons = g.pendingRestart_.reasons;
        }
        if (g.vpnEndpoint_.visible) {
            p.note = g.vpnEndpoint_.summary;
            if (!g.vpnEndpoint_.detail.isEmpty()) p.note += QStringLiteral(" · ") + g.vpnEndpoint_.detail;
            p.noteProblem = g.vpnEndpoint_.problem;
        } else if (g.autoSelector_.visible) {
            p.note = g.autoSelector_.summary;
        }
        return p;
    }
};

namespace Shadowlos::ChromeImpl {

    QString onlineText(qint64 seconds) {
        const qint64 h = seconds / 3600, m = seconds / 60 % 60;
        if (h > 0) return translate("%1 h %2 min").arg(h).arg(m);
        return translate("%1 min").arg(m);
    }

    // The status card: a big status line, a detail line, one more in a wide window (time
    // online, traffic, mode), the restart notice when settings changed, a thin progress bar
    // while busy, the «Подробный журнал» link, and the mascot area at the right edge (an
    // empty placeholder of fixed size for now). Replaces upstream's data view text.
    class StatusCard final : public QFrame {
    public:
        StatusCard(QWidget *parent, Ui::MainWindow *ui) : QFrame(parent), m_ui(ui) {
            setObjectName(QStringLiteral("slStatusPanel"));
            setFrameShape(QFrame::NoFrame);
            auto *row = new QHBoxLayout(this);
            row->setContentsMargins(20, 12, 12, 12);
            row->setSpacing(16);

            auto *text = new QVBoxLayout();
            text->setSpacing(4);
            text->addStretch(1);
            auto *titleRow = new QHBoxLayout();
            titleRow->setSpacing(12);
            m_title = new ElideLabel(this, "statusTitle", translate("Checking servers"));
            titleRow->addWidget(m_title, 1);
            m_titleRow = titleRow;
            text->addLayout(titleRow);
            m_detail = new ElideLabel(this, "statusDetail", translate("Add a config or select one server"));
            text->addWidget(m_detail);
            m_extra = new ElideLabel(this, "statusDetail");
            m_extra->hide();
            text->addWidget(m_extra);
            m_action = new QLabel(this);
            m_action->setProperty("slRole", "statusAction");
            m_action->setTextFormat(Qt::RichText);
            m_action->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
            m_action->hide();
            // The restart notice's links go where upstream's data view sent them.
            QObject::connect(m_action, &QLabel::linkActivated, this, [ui](const QString &link) {
                emit ui->data_view->anchorClicked(QUrl(link));
            });
            text->addWidget(m_action);
            m_progress = new QProgressBar(this);
            m_progress->setObjectName(QStringLiteral("slStatusProgress"));
            m_progress->setTextVisible(false);
            m_progress->setRange(0, 1000);
            m_progress->setFixedHeight(4);
            m_progress->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            m_progress->hide();
            text->addSpacing(2);
            text->addWidget(m_progress);
            m_link = new QToolButton(this);
            m_link->setObjectName(QStringLiteral("slLogLink"));
            m_link->setProperty("slRole", "link");
            m_link->setAutoRaise(true);
            m_link->setCursor(Qt::PointingHandCursor);
            m_link->setText(translate("Detailed log"));
            auto *linkRow = new QHBoxLayout();
            linkRow->setContentsMargins(0, 2, 0, 0);
            linkRow->addWidget(m_link);
            linkRow->addStretch(1);
            text->addLayout(linkRow);
            text->addStretch(1);
            row->addLayout(text, 1);

            // The mascot area at the card's right edge, a fixed size; in the minimal view a quiet
            // «Вид» link sits over it, top right.
            auto *side = new QVBoxLayout();
            side->setSpacing(4);
            side->setContentsMargins(0, 0, 0, 0);
            m_viewSlot = new QHBoxLayout();
            m_viewSlot->setContentsMargins(0, 0, 0, 0);
            m_viewSlot->addStretch(1);
            side->addLayout(m_viewSlot);
            side->addStretch(1);
            m_mascot = new QFrame(this);
            m_mascot->setObjectName(QStringLiteral("slMascot"));
            side->addWidget(m_mascot, 0, Qt::AlignRight);
            side->addStretch(1);
            row->addLayout(side);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            applySizes();

            m_timer = new QTimer(this);
            m_timer->setInterval(500);
            QObject::connect(m_timer, &QTimer::timeout, this, [this] {
                if (!hiddenNow()) refresh();
            });
            m_timer->start();

            // A click on the status or its detail tests the running server, as upstream's
            // status label did (MainWindow's event filter on label_running, url_test_current).
            m_title->installEventFilter(this);
            m_detail->installEventFilter(this);
            // The card follows the button's state at once, not on its next tick: the button
            // sets a new tooltip on every state change (StartStopButton::applyState).
            if (QWidget *power = ui->toolButton_startstop) power->installEventFilter(this);
            // The card and the stopwatch pause while the window is hidden or minimized; when it
            // comes back they catch up at once, so it never shows a state from before.
            if (QWidget *top = parent->window()) top->installEventFilter(this);
        }

        QToolButton *logLink() const { return m_link; }

        // The minimal view's «Вид ⌄», top right over the mascot area.
        void setViewLink(QToolButton *link) { m_viewSlot->addWidget(link); }

        // The narrow layout's minimum, also while wide: the bigger wide fonts and mascot must
        // never set the window's minimum width (the window is to shrink back past them).
        QSize minimumSizeHint() const override {
            if (!m_wide) m_narrowMin = QFrame::minimumSizeHint();
            if (m_narrowMin.isValid()) return {m_narrowMin.width(), QFrame::minimumSizeHint().height()};
            return QFrame::minimumSizeHint();
        }

        // The connect stopwatch (the corner label) at the title row's right end.
        void setStopwatch(QLabel *stopwatch) { m_titleRow->addWidget(stopwatch, 0, Qt::AlignVCenter); }

        bool wide() const { return m_wide; }

        void setWide(bool wide) {
            if (wide == m_wide) return;
            if (wide) m_narrowMin = QFrame::minimumSizeHint();
            m_wide = wide;
            setProperty("slWide", wide);
            for (QWidget *w : {static_cast<QWidget *>(this), static_cast<QWidget *>(m_title), static_cast<QWidget *>(m_detail),
                               static_cast<QWidget *>(m_extra), static_cast<QWidget *>(m_link)}) {
                repolish(w);
            }
            applySizes();
            refresh();
        }

        void setMinimal(bool minimal) {
            m_minimal = minimal;
            setProperty("slMini", minimal);
            for (QWidget *w : {static_cast<QWidget *>(this), static_cast<QWidget *>(m_title), static_cast<QWidget *>(m_detail),
                               static_cast<QWidget *>(m_extra)}) {
                repolish(w);
            }
            m_link->setVisible(!minimal);
            applySizes();
            refresh();
        }

        void refresh() {
            const DataPanel p = generator() != nullptr ? ShadowlosDataViewPeek::read(*generator()) : DataPanel{};
            auto *power = static_cast<StartStopButton *>(m_ui->toolButton_startstop);
            const auto st = power->state();
            const auto *settings = Configs::dataManager->settingsRepo.get();
            QString title, detail, extra;
            bool problem = false;
            qint64 done = 0, total = 0;

            std::shared_ptr<Configs::Profile> profile;
            if (st == StartStopButton::State::Running && settings->started_id >= 0)
                profile = Configs::dataManager->profilesRepo->GetProfile(settings->started_id);

            switch (st) {
                case StartStopButton::State::Disabled:
                    // Nothing to start: an empty group, a fresh install, or several rows selected.
                    title = translate("No server");
                    detail = translate("Add a config or select one server");
                    break;
                case StartStopButton::State::Connecting:
                    title = translate("Connecting");
                    detail = translate("Connecting to the server");
                    total = -1;
                    break;
                case StartStopButton::State::Disconnecting:
                    title = translate("Disconnecting");
                    detail.clear();
                    total = -1;
                    break;
                case StartStopButton::State::Running: {
                    title = translate("Connected");
                    QStringList parts;
                    if (profile && profile->outbound) parts << profile->outbound->DisplayName();
                    if (profile && profile->latency > 0) parts << translate("%1 ms").arg(profile->latency);
                    detail = parts.join(QStringLiteral(" · "));
                    QStringList more;
                    const Corner &c = corner();
                    if (c.since.isValid()) more << translate("Online %1").arg(onlineText(c.since.elapsed() / 1000));
                    if (profile) {
                        const qint64 bytes = profile->traffic_downlink + profile->traffic_uplink;
                        if (bytes > 0) more << translate("traffic %1").arg(shortSize(bytes));
                    }
                    if (const auto route = Configs::dataManager->routesRepo->GetRouteProfile(settings->current_route_id))
                        more << translate("mode «%1»").arg(route->name);
                    extra = more.join(QStringLiteral(" · "));
                    break;
                }
                default:
                    title = translate("Disconnected");
                    detail = translate("Press the button to connect");
                    break;
            }
            if (p.busy) {
                // Connecting and connected keep their title; the test is the detail.
                if (st != StartStopButton::State::Connecting && st != StartStopButton::State::Running) title = p.title;
                detail = p.detail;
                done = p.done;
                total = p.total;
            }
            if (!p.note.isEmpty()) {
                extra = p.note;
                problem = p.noteProblem;
            }
            // A click-to-test of the running server: upstream writes «Проверка» and then «Результат
            // теста: N ms» on label_running for a few seconds; the detail line shows it.
            const QString running = m_ui->label_running->text();
            if (st == StartStopButton::State::Running && isCurrentTest(running)) {
                static const QRegularExpression ms(QStringLiteral("(\\d+) ms$"));
                detail = QString(running).replace(ms, translate("%1 ms").arg(QStringLiteral("\\1")));
            }
            // Select mode (pick a profile for a chain): upstream's prompt, as it was.
            if (running.endsWith(QStringLiteral(" *"))) {
                title = running;
                detail = m_ui->label_running->toolTip();
            }
            // The stopwatch follows the same state as the button and the title: a run that began
            // without the start hook (or a stop without the stop hook) still shows right.
            // Only this card's own stopwatch (the self-test swaps in a throwaway one).
            if (const Corner &c = corner(); !demoHas("on") && c.stopwatch != nullptr && c.stopwatch->parentWidget() == this) {
                if (st == StartStopButton::State::Running && !c.since.isValid()) Shadowlos::Chrome::ConnectStopwatch(true);
                else if ((st == StartStopButton::State::Idle || st == StartStopButton::State::Disabled) && c.since.isValid())
                    Shadowlos::Chrome::ConnectStopwatch(false);
                // Shown only with «Подключено», never next to «Подключаюсь».
                const bool show = st == StartStopButton::State::Running && c.since.isValid();
                if (c.stopwatch->isVisibleTo(this) != show) c.stopwatch->setVisible(show);
            }
            const auto cursor = st == StartStopButton::State::Running ? Qt::PointingHandCursor : Qt::ArrowCursor;
            if (m_title->cursor().shape() != cursor) {
                m_title->setCursor(cursor);
                m_detail->setCursor(cursor);
            }

            bool restart = p.restart;
            if (demoHas("on")) {
                title = translate("Connected");
                detail = QStringLiteral("Германия #2 · ") + translate("%1 ms").arg(48);
                extra = translate("Online %1").arg(onlineText(72 * 60)) + QStringLiteral(" · ") +
                        translate("traffic %1").arg(QStringLiteral("1.2 GB")) + QStringLiteral(" · ") +
                        translate("mode «%1»").arg(QStringLiteral("Удобный"));
                total = 0;
                restart = false;
                problem = false;
                const Corner &c = corner();
                if (c.stopwatch) {
                    c.stopwatch->setText(Shadowlos::Chrome::StopwatchText(72 * 60 + 5));
                    c.stopwatch->setVisible(true);
                }
            } else if (demoHas("busy")) {
                title = translate("Connecting");
                detail = translate("Checking servers: %1").arg(ofText(19, 22));
                extra.clear();
                done = 19;
                total = 22;
                restart = false;
                problem = false;
            } else if (demoHas("off")) {
                title = translate("Disconnected");
                detail = translate("Press the button to connect");
                extra.clear();
                total = 0;
                restart = false;
                problem = false;
            }

            m_title->setFullText(title);
            m_detail->setFullText(detail);
            m_detail->setVisible(!detail.isEmpty());
            m_extra->setFullText(extra);
            // The extra line: in a wide window, or when it carries a note.
            m_extra->setVisible(!extra.isEmpty() && !m_minimal && (m_wide || !p.note.isEmpty()));
            if (m_extra->property("slError").toBool() != problem) {
                m_extra->setProperty("slError", problem);
                repolish(m_extra);
            }

            if (restart) {
                const QString text = QStringLiteral("%1. <a href=\"%2\">%3</a> &nbsp;·&nbsp; <a href=\"%4\">%5</a>")
                                         .arg(translate("Settings changed").toHtmlEscaped(),
                                              QLatin1String(DataViewHtmlGenerator::RestartActionUrl),
                                              translate("Restart").toHtmlEscaped(),
                                              QLatin1String(DataViewHtmlGenerator::DismissRestartActionUrl),
                                              translate("Not now").toHtmlEscaped());
                if (m_action->text() != text) m_action->setText(text);
                m_action->setToolTip(p.reasons.join(QStringLiteral(", ")));
            }
            m_action->setVisible(restart);

            if (total == 0) {
                m_progress->hide();
            } else {
                if (total < 0) {
                    if (m_progress->maximum() != 0) m_progress->setRange(0, 0);
                } else {
                    if (m_progress->maximum() != 1000) m_progress->setRange(0, 1000);
                    m_progress->setValue(static_cast<int>(std::clamp<qint64>(done * 1000 / total, 0, 1000)));
                }
                m_progress->show();
            }
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (watched == window() && watched != this &&
                (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show)) {
                QTimer::singleShot(0, this, [this] {
                    if (hiddenNow()) return;
                    tickStopwatch();
                    refresh();
                });
                return false;
            }
            if (watched == m_ui->toolButton_startstop && event->type() == QEvent::ToolTipChange) {
                QTimer::singleShot(0, this, [this] {
                    if (!hiddenNow()) refresh();
                });
                return false;
            }
            if ((watched == m_title || watched == m_detail) && event->type() == QEvent::MouseButtonPress) {
                // Upstream's filter tests only on a left press while a profile runs.
                if (QWidget *window = m_ui->label_running->window())
                    return static_cast<QObject *>(window)->eventFilter(m_ui->label_running, event);
            }
            return QFrame::eventFilter(watched, event);
        }

    private:
        static bool isCurrentTest(const QString &text) {
            const QString testing = QCoreApplication::translate("MainWindow", "Testing");
            const QString result = QCoreApplication::translate("MainWindow", "Test Result");
            return text == testing || text.startsWith(result + QStringLiteral(": "));
        }

        void applySizes() {
            // Fixed sizes: the mascot drawing will fit these. Wide: the card grows, so does the area.
            // The top row has one fixed height (kTopRowHeight), so the area fits under it.
            const QSize mascot = m_minimal ? QSize(104, 104) : m_wide ? QSize(220, 124) : QSize(104, 104);
            m_mascot->setFixedSize(mascot);
            m_title->setMinimumWidth(0);
        }

        Ui::MainWindow *m_ui;
        QHBoxLayout *m_titleRow = nullptr;
        ElideLabel *m_title = nullptr;
        ElideLabel *m_detail = nullptr;
        ElideLabel *m_extra = nullptr;
        QLabel *m_action = nullptr;
        QProgressBar *m_progress = nullptr;
        QToolButton *m_link = nullptr;
        QFrame *m_mascot = nullptr;
        QHBoxLayout *m_viewSlot = nullptr;
        mutable QSize m_narrowMin;
        QTimer *m_timer = nullptr;
        bool m_wide = false;
        bool m_minimal = false;
    };

    // ---------------------------------------------------------------- views

    enum class View { Minimal = 0, Normal = 1, Advanced = 2 };

    QString viewName(View v) {
        switch (v) {
            case View::Minimal: return translate("Minimal");
            case View::Advanced: return translate("Advanced");
            default: return translate("Normal");
        }
    }

    QString viewLine(View v) {
        switch (v) {
            case View::Minimal: return translate("button and status only");
            case View::Advanced: return translate("all Throne settings");
            default: return translate("servers and the main settings");
        }
    }

    // The saved view; before 1.0.13 only shadowlos_simple_view existed (on = Обычный).
    View savedView() {
        const auto *settings = Configs::dataManager->settingsRepo.get();
        if (settings->shadowlos_view >= 0 && settings->shadowlos_view <= 2) return static_cast<View>(settings->shadowlos_view);
        return settings->shadowlos_simple_view != 0 ? View::Normal : View::Advanced;
    }

    // One entry of the «Вид» list: the name over a short line. A click picks it.
    class ViewItem final : public QFrame {
    public:
        ViewItem(QWidget *parent, View view, std::function<void(View)> pick)
            : QFrame(parent), m_view(view), m_pick(std::move(pick)) {
            setObjectName(QStringLiteral("slViewItem"));
            setAttribute(Qt::WA_Hover, true);
            setCursor(Qt::PointingHandCursor);
            auto *layout = new QVBoxLayout(this);
            layout->setContentsMargins(12, 6, 24, 6);
            layout->setSpacing(1);
            auto *name = new QLabel(viewName(view), this);
            name->setProperty("slRole", "viewItemName");
            auto *line = new QLabel(viewLine(view), this);
            line->setProperty("slRole", "viewItemLine");
            layout->addWidget(name);
            layout->addWidget(line);
            setAccessibleName(viewName(view));
            // The menu moves focus to the item under its arrow keys; Enter or Space picks it.
            setFocusPolicy(Qt::StrongFocus);
        }

        View view() const { return m_view; }

        // The menu closes first: picking Минимальный resizes the window under it.
        void choose() {
            if (auto *menu = qobject_cast<QMenu *>(parentWidget())) menu->close();
            const auto pick = m_pick;
            const View v = m_view;
            QTimer::singleShot(0, qApp, [pick, v] { pick(v); });
        }

    protected:
        void mouseReleaseEvent(QMouseEvent *event) override {
            if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) choose();
            QFrame::mouseReleaseEvent(event);
        }

        void keyPressEvent(QKeyEvent *event) override {
            switch (event->key()) {
                case Qt::Key_Return:
                case Qt::Key_Enter:
                case Qt::Key_Space:
                    choose();
                    event->accept();
                    return;
                default:
                    QFrame::keyPressEvent(event);
            }
        }

    private:
        View m_view;
        std::function<void(View)> m_pick;
    };

    // The main window's parts that the views show and hide.
    struct Shell {
        QPointer<QMainWindow> window;
        Ui::MainWindow *ui = nullptr;
        QPointer<QWidget> central, menuCard, actionsCard, modesCard, powerCard, bottomBar, filler, speedSep;
        QPointer<QToolButton> viewButton, chip;
        QPointer<StatusCard> status;
        QList<QPointer<ViewItem>> items;
        QList<QPointer<QToolButton>> pills;
        QSize designMin;
        bool logOpen = false;
        bool applied = false;
        View view = View::Normal;
    };

    Shell &shell() {
        static Shell s;
        return s;
    }

    QToolButton *makeViewButton(QWidget *parent, const char *name, QMenu *menu) {
        auto *button = new QToolButton(parent);
        button->setObjectName(QLatin1String(name));
        button->setProperty("slRole", "menu");
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setCursor(Qt::PointingHandCursor);
        button->setMenu(menu);
        repolish(button);
        return button;
    }

    // ------------------------------------------------------------ wide window

    // Below the width the labelled quick actions need, they show only their icons (the name
    // and what they do stay in the tooltip); above it the status card is wide too: a bigger
    // mascot area and the extra detail line.
    class WideWatch final : public QObject {
    public:
        explicit WideWatch(QWidget *central) : QObject(central), m_central(central) {
            central->installEventFilter(this);
        }

        void relayout() {
            Shell &s = shell();
            if (m_central.isNull() || s.status.isNull()) return;
            if (s.view == View::Minimal) return;
            const bool wasWide = m_central->property("slWide").toBool();
            if (!wasWide) {
                // Measured while narrow: what the labelled row needs with the wide fonts.
                int labelled = 0;
                for (const auto &pill : s.pills) {
                    if (!pill.isNull()) labelled = std::max(labelled, wideLabelledWidth(pill.data()));
                }
                int needed = kRowGaps + kActionsFrame + 2 * labelled + kStatusWideMin;
                for (QWidget *card : {s.powerCard.data(), s.modesCard.data()}) {
                    if (card != nullptr && !card->isHidden()) needed += card->sizeHint().width();
                }
                m_needed = needed;
            } else {
                // While wide, what the wide cards really ask for; kept, so the next switch to
                // wide waits for that width too (no flapping between the two).
                int needed = kRowGaps + kStatusWideMin;
                for (QWidget *card : {s.powerCard.data(), s.actionsCard.data(), s.modesCard.data()}) {
                    if (card != nullptr && !card->isHidden()) needed += card->sizeHint().width();
                }
                m_neededWide = std::max(m_neededWide, needed);
            }
            // A little hysteresis, so a width right at the threshold does not flip back and forth.
            const int threshold = std::max(m_needed, m_neededWide);
            const bool wide = m_central->width() >= (wasWide ? threshold - kHysteresis : threshold);
            if (wasWide != wide) {
                // The top row's text grows with the window (shadowlos.qss, [slWide="true"]).
                // The toggle names keep their narrow width as the minimum, so the bigger text
                // never sets the window's minimum width.
                for (QLabel *label : m_central->findChildren<QLabel *>()) {
                    if (label->property("slRole").toString() != QLatin1String("toggleLabel")) continue;
                    label->setMinimumWidth(wide ? label->minimumSizeHint().width() : 0);
                }
                // Narrow: the two cards are exactly as wide as their content. Wide: no wider than
                // that (Maximum), and their minimum is still the narrow one, so the window's
                // minimum width never follows the labels or the bigger text.
                const auto policy = wide ? QSizePolicy::Maximum : QSizePolicy::Fixed;
                for (QWidget *card : {s.actionsCard.data(), s.modesCard.data()}) {
                    if (card != nullptr) card->setSizePolicy(policy, QSizePolicy::Preferred);
                }
                m_central->setProperty("slWide", wide);
                for (const auto &pill : s.pills) {
                    if (!pill.isNull()) repolish(pill);
                }
                for (QLabel *label : m_central->findChildren<QLabel *>()) {
                    if (label->property("slRole").toString() != QLatin1String("toggleLabel")) continue;
                    setToggleText(label, wide);
                    repolish(label);
                }
            }
            const auto style = wide ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly;
            for (const auto &pill : s.pills) {
                if (!pill.isNull() && pill->toolButtonStyle() != style) pill->setToolButtonStyle(style);
            }
            s.status->setWide(wide);
        }

        // Another view has other cards (and the bottom bar), so the wide need is measured anew.
        void resetWide() { m_neededWide = 0; }

        // The window's minimum follows what the shown parts need (a view switch, a font change),
        // never below the .ui's floor outside the minimal view.
        void updateMinimum() {
            Shell &s = shell();
            if (s.window.isNull()) return;
            const QSize hint = s.window->minimumSizeHint();
            const QSize floor = s.view == View::Minimal ? QSize(0, 0) : s.designMin;
            const QSize min(qMax(floor.width(), hint.width()), qMax(floor.height(), hint.height()));
            if (s.window->minimumSize() != min) {
                s.window->setMinimumSize(min);
                if (s.window->width() < min.width() || s.window->height() < min.height())
                    s.window->resize(s.window->size().expandedTo(min));
            }
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (watched != m_central.data()) return false;
            if (event->type() == QEvent::Resize) relayout();
            if (event->type() == QEvent::LayoutRequest && !m_minPending) {
                m_minPending = true;
                QTimer::singleShot(0, this, [this] {
                    m_minPending = false;
                    updateMinimum();
                });
            }
            return false;
        }

    private:
        static constexpr int kStatusWideMin = 460; // the text and the wide mascot area
        // Root margins and the gaps between the row's four cards; the quick actions card's
        // side margins and the gap between its two columns.
        static constexpr int kRowGaps = 24 + 3 * 8;
        static constexpr int kActionsFrame = 32 + 8;
        static constexpr int kHysteresis = 16;
        bool m_minPending = false;
        int m_needed = 0;
        int m_neededWide = 0;

        // As the sheet lays a labelled pill out in a wide window (15px text, 12/16px padding):
        // padding, icon, icon-text gap, text, padding.
        static int wideLabelledWidth(const QToolButton *pill) {
            QFont font = pill->font();
            font.setPixelSize(kWidePillFontPx);
            return 12 + pill->iconSize().width() + 6 + QFontMetrics(font).horizontalAdvance(pill->text()) + 16 + 4;
        }

        QPointer<QWidget> m_central;
    };

    QPointer<WideWatch> &wideWatch() {
        static QPointer<WideWatch> w;
        return w;
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

    QString StopwatchText(qint64 seconds) {
        if (seconds < 0) seconds = 0;
        const qint64 h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
        const auto two = [](qint64 n) { return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0')); };
        if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(two(m), two(s));
        return QStringLiteral("%1:%2").arg(two(m), two(s));
    }

    void ConnectStopwatch(bool connected) {
        using namespace Shadowlos::ChromeImpl;
        Corner &c = corner();
        if (c.stopwatch == nullptr || c.timer == nullptr) return;
        if (connected) {
            c.since.start();
            tickStopwatch();
            c.stopwatch->setVisible(true);
            c.timer->start();
        } else {
            c.timer->stop();
            c.since.invalidate();
            c.stopwatch->clear();
            c.stopwatch->setVisible(false);
        }
        qDebug().noquote() << "[corner] stopwatch" << (connected ? "started" : "reset");
    }

    QString StopwatchSelfTest() {
        using namespace Shadowlos::ChromeImpl;
        QStringList problems;
        const auto expect = [&](const QString &what, const QString &got, const QString &want) {
            if (got != want) problems << QStringLiteral("%1: got '%2', want '%3'").arg(what, got, want);
        };
        const struct { qint64 secs; const char *want; } cases[] = {
            {-5, "00:00"}, {0, "00:00"}, {9, "00:09"}, {61, "01:01"}, {3599, "59:59"},
            {3600, "1:00:00"}, {3661, "1:01:01"}, {36000 + 59, "10:00:59"},
        };
        for (const auto &c : cases) expect(QStringLiteral("text %1").arg(c.secs), StopwatchText(c.secs), QString::fromLatin1(c.want));

        // The timer on a throwaway card: hidden until connected, counts each second, resets on stop.
        // The real window's corners are put back afterwards, should it already exist.
        const Corner saved = corner();
        QWidget card;
        Corner &c = makeCorners(&card);
        expect(QStringLiteral("version"), c.version->text(), QStringLiteral(NKR_VERSION));
        const auto state = [&] {
            return QStringLiteral("%1/%2/%3").arg(c.stopwatch->isVisibleTo(&card) ? "shown" : "hidden",
                                                   c.timer->isActive() ? "ticking" : "idle", c.stopwatch->text());
        };
        expect(QStringLiteral("before connect"), state(), QStringLiteral("hidden/idle/"));
        ConnectStopwatch(true);
        expect(QStringLiteral("on connect"), state(), QStringLiteral("shown/ticking/00:00"));
        QEventLoop loop;
        QTimer::singleShot(2300, &loop, &QEventLoop::quit);
        loop.exec();
        // A busy machine (right after a build) may run the loop late: 00:03 is fine too.
        if (const QString got = state(); got != QStringLiteral("shown/ticking/00:03"))
            expect(QStringLiteral("after 2.3 s"), got, QStringLiteral("shown/ticking/00:02"));
        ConnectStopwatch(false);
        expect(QStringLiteral("on stop"), state(), QStringLiteral("hidden/idle/"));
        ConnectStopwatch(true);
        expect(QStringLiteral("reconnect"), state(), QStringLiteral("shown/ticking/00:00"));
        ConnectStopwatch(false);
        corner() = saved;
        return problems.join(QStringLiteral("; "));
    }

    QString UiDemo() {
#if SHADOWLOS_DEV
        static const QString demo = qEnvironmentVariable("SHADOWLOS_UI_DEMO").trimmed().toLower();
        return demo;
#else
        return {};
#endif
    }

    void DataViewChanged(DataViewHtmlGenerator *generator) {
        ChromeImpl::generator() = generator;
        if (auto &status = ChromeImpl::shell().status; status && !ChromeImpl::hiddenNow()) status->refresh();
    }

    QString StatusLine(const QString &text) {
        QString line = text;
        line.replace(QLatin1Char('\n'), QStringLiteral("  |  "));
        return line;
    }

    // ----------------------------------------------------------- simple view

    // Обычный (the default) lists only what a user picks a server by: name, test result and
    // traffic, in big rows, without the logs/connections panel. Продвинутый is upstream's full
    // view with dense rows. Минимальный has no list at all.
    bool simpleViewOn() {
        return ChromeImpl::shell().view != ChromeImpl::View::Advanced;
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
        // The sheet sets a bigger font and a left indent on the rows in the simple view.
        table->setProperty("slSimple", simple);
        repolish(table);
        repolish(table->horizontalHeader());
        // Column titles over their values: the simple view's last column is wide.
        table->horizontalHeader()->setDefaultAlignment(simple ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
        table->verticalHeader()->setDefaultSectionSize(simple ? 64 : kDenseRowPx);
        // The logs/connections panel: always in Продвинутый, in Обычный while «Подробный журнал» is open.
        if (QWidget *panel = ui->splitter->widget(1)) panel->setVisible(!simple || shell().logOpen);
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

    // The full view's saved geometry, kept on screen; a default size when there is none.
    void restoreFullGeometry(QMainWindow *window) {
        const QByteArray saved = QByteArray::fromBase64(Configs::dataManager->settingsRepo->shadowlos_full_geometry.toLatin1());
        if (saved.isEmpty() || !window->restoreGeometry(saved)) {
            window->resize(1100, 720);
        }
        if (window->isMaximized() || window->screen() == nullptr) return;
        const QRect area = window->screen()->availableGeometry();
        QRect frame = window->frameGeometry();
        const QSize extra = frame.size() - window->size();
        const QSize fit = (area.size() - extra).boundedTo(window->size()).expandedTo(window->minimumSize());
        if (fit != window->size()) window->resize(fit);
        frame = window->frameGeometry();
        QPoint at = frame.topLeft();
        at.setX(std::clamp(at.x(), area.left(), std::max(area.left(), area.right() - frame.width() + 1)));
        at.setY(std::clamp(at.y(), area.top(), std::max(area.top(), area.bottom() - frame.height() + 1)));
        if (at != frame.topLeft()) window->move(at);
    }

    // Shows one of the three views. save: remember it (a pick from the «Вид» list); the
    // dev screenshot switch does not.
    void applyView(ChromeImpl::View view, bool save) {
        using namespace Shadowlos::ChromeImpl;
        Shell &s = shell();
        QMainWindow *window = s.window;
        Ui::MainWindow *ui = s.ui;
        if (window == nullptr || ui == nullptr) return;
        const bool first = !s.applied;
        const View before = s.view;
        s.applied = true;
        s.view = view;
        if (save) {
            auto *settings = Configs::dataManager->settingsRepo.get();
            settings->shadowlos_view = static_cast<int>(view);
            settings->shadowlos_simple_view = view == View::Advanced ? 0 : 1; // for an older build
            settings->Save();
        }
        const bool mini = view == View::Minimal;
        auto *settings = Configs::dataManager->settingsRepo.get();
        // Leaving a full view: its geometry (size, place, maximized) comes back when the minimal
        // view is left, also after a restart in the minimal view.
        if (mini && !first && before != View::Minimal) {
            settings->shadowlos_full_geometry = QString::fromLatin1(window->saveGeometry().toBase64());
            settings->Save();
        }
        window->setProperty("slCompactWindow", mini);

        for (QWidget *w : {s.menuCard.data(), s.actionsCard.data(), s.modesCard.data(), s.bottomBar.data(),
                           static_cast<QWidget *>(ui->splitter)}) {
            if (w != nullptr) w->setVisible(!mini);
        }
        if (s.filler) s.filler->setVisible(mini);
        if (s.chip) s.chip->setVisible(mini);
        ui->toolButton_startstop->setFixedSize(mini ? 96 : 64, mini ? 96 : 64);
        if (s.powerCard) s.powerCard->setFixedWidth(mini ? kPowerCardWidthMini : kPowerCardWidth);
        const QString label = translate("View: %1").arg(viewName(view));
        if (s.viewButton) s.viewButton->setText(label);
        if (s.chip) s.chip->setToolTip(label);
        for (const auto &item : s.items) {
            if (item.isNull()) continue;
            item->setProperty("slCurrent", item->view() == view);
            repolish(item);
        }
        // Traffic speed only in Продвинутый.
        ui->label_speed->ensurePolished();
        ui->label_speed->setMinimumWidth(ui->label_speed->fontMetrics().horizontalAdvance(
            StatusLine(QObject::tr("Proxy: %1\nDirect: %2").arg(QStringLiteral("99.9 KiB↑ 99.9 MiB↓"), QStringLiteral("99.9 KiB↑ 99.9 KiB↓")))) + 8);
        ui->label_speed->setVisible(view == View::Advanced);
        if (s.speedSep) s.speedSep->setVisible(view == View::Advanced);
        if (s.status) {
            s.status->setMinimal(mini);
            s.status->logLink()->setText(view == View::Normal && s.logOpen ? translate("Hide log") : translate("Detailed log"));
        }
        applySimpleView(ui);

        // The window: the minimal view as small as it can be; a full view gets its size back.
        if (s.central && s.central->layout()) s.central->layout()->activate();
        if (window->layout()) window->layout()->activate();
        if (mini) {
            if (window->isMaximized() || window->isFullScreen()) window->showNormal();
            window->setMinimumSize(0, 0);
            const QSize hint = window->minimumSizeHint();
            window->setMinimumSize(hint);
            window->resize(hint.expandedTo(QSize(560, 0)));
        } else {
            const QSize hint = window->minimumSizeHint();
            window->setMinimumSize(qMax(s.designMin.width(), hint.width()), qMax(s.designMin.height(), hint.height()));
            // Back from the minimal view, or the last run ended in it.
            if ((!first && before == View::Minimal) || (first && window->width() < 700)) restoreFullGeometry(window);
        }
        if (auto w = wideWatch()) {
            if (view != before) w->resetWide();
            w->relayout();
        }
        qDebug().noquote() << "[view]" << viewName(view);
    }

    // «Подробный журнал»: the Logs tab. Продвинутый has the panel; Обычный opens it under the
    // list and closes it again on a second press.
    void openLog(Ui::MainWindow *ui) {
        using namespace Shadowlos::ChromeImpl;
        Shell &s = shell();
        QWidget *panel = ui->splitter->widget(1);
        if (panel == nullptr) return;
        if (s.view == View::Normal) {
            s.logOpen = !s.logOpen;
            applyView(s.view, false);
            if (!s.logOpen) return;
        }
        for (int i = 0; i < ui->stats_widget->count(); ++i) {
            QWidget *page = ui->stats_widget->widget(i);
            if (page != nullptr && page->isAncestorOf(ui->masterLogBrowser)) ui->stats_widget->setCurrentIndex(i);
        }
        if (ui->splitter->sizes().value(1) < ui->stats_widget->tabBar()->sizeHint().height() * 3) {
            const int height = ui->splitter->size().height();
            ui->splitter->setSizes({height * 3 / 5, height * 2 / 5});
        }
    }

    // Dev screenshots: shows the «Вид» list or a quick action's tooltip once the window is on
    // screen (the test restores it without activating it). Both as tool windows that take
    // neither focus nor the mouse.
    void installDemoPopups(QMainWindow *window, Ui::MainWindow *ui) {
        using namespace Shadowlos::ChromeImpl;
        if (!demoHas("menu") && !demoHas("tip")) return;
        auto *poll = new QTimer(window);
        poll->setInterval(300);
        QObject::connect(poll, &QTimer::timeout, window, [window, ui, poll] {
            if (!window->isVisible() || window->isMinimized()) return;
            poll->stop();
            QTimer::singleShot(1200, window, [window, ui] {
                Shell &s = shell();
                if (demoHas("menu") && s.viewButton) {
                    QToolButton *button = s.view == View::Minimal ? s.chip.data() : s.viewButton.data();
                    QMenu *menu = button->menu();
                    menu->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
                    menu->setAttribute(Qt::WA_TranslucentBackground, true);
                    const QSize size = menu->sizeHint();
                    menu->move(button->mapToGlobal(QPoint(button->width() - size.width(), button->height() + 4)));
                    menu->show();
                }
                if (demoHas("tip")) {
                    for (const auto &pill : s.pills) {
                        if (pill.isNull() || pill->objectName() != QLatin1String("slSpeedtest")) continue;
                        QToolTip::showText(pill->mapToGlobal(QPoint(0, pill->height() + 6)), pill->toolTip(), pill, {}, 20000);
                    }
                }
                Q_UNUSED(ui)
            });
        });
        poll->start();
    }

    // A 24px pause or play glyph in the accent blue, drawn at 2x.
    QIcon logGlyph(bool pause) {
        QPixmap pixmap(48, 48);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Shadowlos::ChromeImpl::hex(0x33A5FF));
        if (pause) {
            painter.drawRoundedRect(QRectF(13, 10, 8, 28), 2, 2);
            painter.drawRoundedRect(QRectF(27, 10, 8, 28), 2, 2);
        } else {
            painter.drawPolygon(QPolygonF({QPointF(15, 10), QPointF(38, 24), QPointF(15, 38)}));
        }
        painter.end();
        pixmap.setDevicePixelRatio(2);
        return QIcon(pixmap);
    }

    // Pause / resume for the log, in the corner of the Logs / Apps tabs while Logs is open.
    // Paused, new lines still arrive but the view holds still (MainWindow::flush_log_batch
    // reads slLogPaused); resuming jumps to the newest line.
    void installLogPause(Ui::MainWindow *ui) {
        using namespace Shadowlos::ChromeImpl;
        auto *tabs = ui->stats_widget;
        auto *log = ui->masterLogBrowser;
        auto *button = new QToolButton(tabs);
        button->setObjectName(QStringLiteral("slLogPause"));
        button->setProperty("slRole", "pill");
        button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setCursor(Qt::PointingHandCursor);
        const auto sync = [button, log] {
            const bool paused = log->property("slLogPaused").toBool();
            button->setText(paused ? translate("Resume log") : translate("Pause log"));
            button->setIcon(logGlyph(!paused));
        };
        sync();
        repolish(button);
        tabs->setCornerWidget(button, Qt::TopRightCorner);
        button->show();
        QObject::connect(button, &QToolButton::clicked, button, [log, sync] {
            const bool paused = !log->property("slLogPaused").toBool();
            log->setProperty("slLogPaused", paused);
            sync();
            if (!paused) log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
        });
        const auto showForLog = [tabs, log, button](int index) {
            QWidget *page = tabs->widget(index);
            button->setVisible(page != nullptr && page->isAncestorOf(log));
        };
        QObject::connect(tabs, &QTabWidget::currentChanged, button, showForLog);
        showForLog(tabs->currentIndex());
    }

    // The startup test measures the servers the group has when it starts. The subscription
    // update runs at the same time and may add servers a second later (1.0.13-dev: three new
    // ones at the top of the list, «Результат теста» empty). So once that test is done, the
    // servers added to that same group since then are measured, those alone. Everything the
    // group held when the startup test began, or when a follow-up began, counts as asked for:
    // a server whose result a stopped test cleared is not tested again behind the user's back.
    // «Остановить тесты» ends the follow-up, and so do ten minutes.
    class UntestedFollowUp final : public QObject {
    public:
        UntestedFollowUp(MainWindow *window, Ui::MainWindow *ui) : QObject(ui->profilesTableView), m_window(window), m_ui(ui) {
            m_timer.setSingleShot(true);
            m_timer.setInterval(3000);
            QObject::connect(&m_timer, &QTimer::timeout, this, [this] { check(); });
            if (auto *model = ui->profilesTableView->model()) {
                QObject::connect(model, &QAbstractItemModel::modelReset, this, [this] { m_timer.start(); });
                QObject::connect(model, &QAbstractItemModel::rowsInserted, this, [this] { m_timer.start(); });
                QObject::connect(model, &QAbstractItemModel::layoutChanged, this, [this] { m_timer.start(); });
            }
            QObject::connect(ui->menu_stop_testing, &QAction::triggered, this, [this] {
                qDebug().noquote() << "[startup] tests stopped: no follow-up test";
                deleteLater();
            });
            if (const auto group = Configs::dataManager->groupsRepo->CurrentGroup()) {
                m_gid = group->id;
                for (int id : group->Profiles()) m_asked.insert(id);
            }
            m_stop.setSingleShot(true);
            QObject::connect(&m_stop, &QTimer::timeout, this, [this] { deleteLater(); });
            m_stop.start(10 * 60 * 1000); // only around startup
        }

    private:
        void check() {
            if (m_window.isNull() || m_gid < 0) return;
            const auto group = Configs::dataManager->groupsRepo->GetGroup(m_gid);
            if (group == nullptr) return;
            QList<int> fresh;
            for (int id : group->Profiles()) {
                if (m_asked.contains(id)) continue;
                const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
                if (profile && profile->outbound && profile->latency == 0) fresh << id;
            }
            if (fresh.isEmpty()) return;
            // Not while a test holds the runner (it would answer with a modal), a panel is busy,
            // the core is down, or a connection is being made or taken down: look again later.
            const auto st = static_cast<StartStopButton *>(m_ui->toolButton_startstop)->state();
            const bool busy = !Configs::dataManager->settingsRepo->core_running || m_window->ShadowlosLatencyTestBusy() ||
                              (ChromeImpl::generator() != nullptr && ShadowlosDataViewPeek::read(*ChromeImpl::generator()).busy) ||
                              st == StartStopButton::State::Connecting || st == StartStopButton::State::Disconnecting;
            if (busy) {
                m_timer.start();
                return;
            }
            // From here on the whole group as it is now counts as asked for.
            for (int id : group->Profiles()) m_asked.insert(id);
            qDebug().noquote() << "[startup] measuring" << fresh.size() << "server(s) added after the startup test";
            m_window->ShadowlosUrlTest(fresh);
        }

        QPointer<MainWindow> m_window;
        Ui::MainWindow *m_ui;
        int m_gid = -1;
        QTimer m_timer;
        QTimer m_stop;
        QSet<int> m_asked;
    };

    void installUntestedFollowUp(Ui::MainWindow *ui) {
        if (auto *window = qobject_cast<MainWindow *>(ui->profilesTableView->window())) new UntestedFollowUp(window, ui);
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
            QTimer::singleShot(1500, ui->actionUrl_Test_Group, [ui] {
                ui->actionUrl_Test_Group->trigger();
                installUntestedFollowUp(ui);
            });
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

        Shell &s = shell();
        s.window = window;
        s.ui = ui;
        uiHidden() = [] {
            QMainWindow *w = shell().window;
            return w == nullptr || !w->isVisible() || w->isMinimized();
        };
        s.designMin = window->minimumSize(); // the .ui's; MainWindow reads it later the same way
        s.view = savedView();
        if (Configs::dataManager->settingsRepo->shadowlos_view < 0) {
            Configs::dataManager->settingsRepo->shadowlos_view = static_cast<int>(s.view);
            Configs::dataManager->settingsRepo->Save();
        }

        auto *central = new QWidget(window);
        central->setObjectName(QStringLiteral("slCentral"));
        s.central = central;
        auto *root = new QVBoxLayout(central);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(12);

        // The «Вид» list, shared by the menu bar's button and the minimal view's chip.
        auto *viewMenu = new QMenu(central);
        viewMenu->setObjectName(QStringLiteral("slViewMenu"));
        for (View v : {View::Minimal, View::Normal, View::Advanced}) {
            auto *action = new QWidgetAction(viewMenu);
            auto *item = new ViewItem(viewMenu, v, [](View picked) { applyView(picked, true); });
            action->setDefaultWidget(item);
            // Enter on the highlighted entry when the item itself does not have focus.
            QObject::connect(action, &QAction::triggered, item, [item] { item->choose(); });
            viewMenu->addAction(action);
            s.items << item;
        }

        // Menu bar: Программа / Настройки / Группы / Маршруты / Утилиты on the left; «Вид» and
        // «Обновить» on the right, the updater's status line just left of them.
        auto *menuCard = makeCard(central, "slMenuCard");
        s.menuCard = menuCard;
        menuCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *menuRow = new QHBoxLayout(menuCard);
        menuRow->setContentsMargins(8, 4, 8, 4);
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
        auto *appUpdate = makePill(menuCard, "slAppUpdate", translate("Update app"), "refresh");
        appUpdate->setCursor(Qt::PointingHandCursor);
        appUpdate->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); // never squeezed to its icon
        // «Обновить»: always pressable, an accent while a verified update is ready, its outcome
        // on a status line that takes only free room (ShadowlosUpdater), else hangs under it.
        menuRow->addWidget(Shadowlos::Updater::Install(window, appUpdate));
        s.viewButton = makeViewButton(menuCard, "slViewButton", viewMenu);
        menuRow->addWidget(s.viewButton);
        menuRow->addWidget(appUpdate);
        root->addWidget(menuCard);
        // Read by MainWindow::applyTopBarMetrics: the design sizes these to their labels.
        window->setProperty("slVariableWidthMenus", true);

        // The top row: one fixed height in every window size (it never takes the list's room),
        // the cards side by side. Power, quick actions and toggles keep their own widths; the
        // status card takes the rest.
        auto *topRow = new QWidget(central);
        topRow->setObjectName(QStringLiteral("slTopRow"));
        topRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        topRow->setMinimumHeight(kTopRowHeight);
        auto *actionsRow = new QHBoxLayout(topRow);
        actionsRow->setContentsMargins(0, 0, 0, 0);
        actionsRow->setSpacing(8);

        // Power card: the button alone, centred both ways, in a card of fixed width (set per
        // view). The new button replaces uic's StartStopButton before anything connects to it;
        // MainWindow only ever reaches it through ui->toolButton_startstop.
        auto *powerCard = makeCard(central, "slCard");
        s.powerCard = powerCard;
        powerCard->setFixedWidth(kPowerCardWidth);
        auto *powerLayout = new QGridLayout(powerCard);
        powerLayout->setContentsMargins(0, 0, 0, 0);
        auto *power = new PowerButton(powerCard);
        power->setObjectName(QStringLiteral("toolButton_startstop"));
        powerLayout->addWidget(power, 0, 0, Qt::AlignCenter);
        if (StartStopButton *old = ui->toolButton_startstop) {
            ui->horizontalLayout_2->removeWidget(old);
            delete old;
        }
        ui->toolButton_startstop = power;
        actionsRow->addWidget(powerCard);

        // Quick actions: always a 2x2 grid of fixed-height buttons, centred in the card's height;
        // icons only in a narrow window (WideWatch). The card is no wider than its buttons
        // need; while wide its minimum stays the icon-only grid (WideWatch), so the labels
        // never set the window's minimum width.
        auto *actionsCard = makeCard(central, "slCard");
        s.actionsCard = actionsCard;
        actionsCard->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        auto *actionsLayout = new QGridLayout(actionsCard);
        actionsLayout->setContentsMargins(16, 0, 16, 0);
        actionsLayout->setSpacing(8);
        actionsLayout->setRowStretch(0, 1);
        actionsLayout->setRowStretch(3, 1);

        auto *add = makePill(actionsCard, "slAddConfig", translate("Add config"), "add-config");
        add->setPopupMode(QToolButton::InstantPopup);
        auto *addMenu = new QMenu(add);
        addMenu->addAction(ui->menu_add_from_clipboard);
        addMenu->addAction(ui->menu_add_from_input);
        addMenu->addAction(ui->actionAdd_profile_from_File);
        addMenu->addAction(ui->menu_scan_qr);
        add->setMenu(addMenu);
        setPillTip(add, add->text(), translate("Adds a server from the clipboard, a link, a file or a QR code."));

        ui->horizontalLayout_2->removeWidget(ui->toolButton_update_subs);
        stylePill(ui->toolButton_update_subs, "refresh");
        setPillTip(ui->toolButton_update_subs, ui->toolButton_update_subs->text(),
                   translate("Downloads the latest server list from your subscription."));

        auto *speedtest = makeActionPill(actionsCard, "slSpeedtest", translate("Measure speed"), "chart",
                                         ui->actionSpeedtest_Group);
        setPillTip(speedtest, speedtest->text(), translate("Checks the speed of every server in the list. Takes up to a minute."));
        auto *latency = makeActionPill(actionsCard, "slLatency", translate("Measure latency"), "latency",
                                       ui->actionUrl_Test_Group);
        setPillTip(latency, latency->text(), translate("Checks which servers answer and how fast."));
        actionsLayout->addWidget(add, 1, 0);
        actionsLayout->addWidget(ui->toolButton_update_subs, 1, 1);
        actionsLayout->addWidget(speedtest, 2, 0);
        actionsLayout->addWidget(latency, 2, 1);
        for (QToolButton *pill : {add, ui->toolButton_update_subs, speedtest, latency}) {
            pill->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            pill->setFixedHeight(kPillHeight);
            s.pills << pill;
        }
        actionsRow->addWidget(actionsCard);

        // Debug is rarely used, so it lives in the Tools menu instead of the row.
        ui->horizontalLayout_2->removeWidget(ui->toolButton_debug);
        ui->toolButton_debug->hide();
        auto *debugAction = new QAction(ui->toolButton_debug->text(), ui->menuTools);
        QObject::connect(debugAction, &QAction::triggered, ui->toolButton_debug, &QToolButton::click);
        ui->menuTools->addSeparator();
        ui->menuTools->addAction(debugAction);
        // Dev builds: the error journal's consent action goes under it (a no-op in prod).
        Shadowlos::Journal::Install(window, ui->menuTools, ui->checkBox_VPN, ui->checkBox_SystemProxy);
        // Dev builds: «Улучшение обхода блокировок» at the end of Утилиты (the menu bar is
        // there in Обычный and Продвинутый). Nothing in prod.
        Shadowlos::Journal::InstallImprovePage(window, ui->menuTools);

        // Toggles card: the modes, and in dev builds the journal consent as «Улучшение обхода».
        auto *modesCard = makeCard(central, "slCard");
        s.modesCard = modesCard;
        // As wide as its rows; while wide its minimum stays the narrow one (WideWatch).
        modesCard->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        auto *modesLayout = new QHBoxLayout(modesCard);
        modesLayout->setContentsMargins(16, 8, 16, 8);
        modesLayout->setSpacing(16);
        auto *toggles = new QVBoxLayout();
        toggles->setSpacing(10);
        toggles->addStretch(1);
        for (QCheckBox *box : {ui->checkBox_VPN, ui->checkBox_SystemProxy, ui->system_dns}) {
            ui->verticalLayout_4->removeWidget(box);
            toggles->addWidget(makeToggleRow(modesCard, box));
        }
#if SHADOWLOS_DEV
        if (QAction *consent = Shadowlos::Journal::ConsentAction()) {
            auto *box = new QCheckBox(QStringLiteral("Улучшение обхода (отчёты о блокировках)"), modesCard);
            box->setObjectName(QStringLiteral("slImproveToggle"));
            box->setToolTip(consent->text());
            box->setChecked(consent->isChecked());
            QObject::connect(box, &QCheckBox::toggled, consent, [consent](bool on) {
                if (consent->isChecked() != on) consent->setChecked(on);
            });
            QObject::connect(consent, &QAction::toggled, box, [box](bool on) {
                if (box->isChecked() != on) box->setChecked(on);
            });
            toggles->addWidget(makeToggleRow(modesCard, box));
        }
#endif
        toggles->addStretch(1);
        modesLayout->addLayout(toggles);
        actionsRow->addWidget(modesCard);

        // Status card, in place of upstream's data view text. The data view stays (hidden): its
        // links and setHtml calls keep working, and the card reads the same panels.
        ui->horizontalLayout_2->removeWidget(ui->data_view);
        ui->data_view->setParent(central);
        ui->data_view->hide();
        auto *status = new StatusCard(central, ui);
        s.status = status;
        QObject::connect(status->logLink(), &QToolButton::clicked, window, [ui] { openLog(ui); });
        actionsRow->addWidget(status, 1);
        // Минимальный: a quiet «Вид ⌄» at the status card's top right; the power card keeps
        // only the button.
        s.chip = makeViewButton(status, "slViewChip", viewMenu);
        s.chip->setProperty("slRole", "viewChip");
        s.chip->setText(translate("View"));
        s.chip->setToolTip(translate("View: %1").arg(viewName(s.view)));
        s.chip->setAccessibleName(translate("View"));
        repolish(s.chip);
        s.chip->hide();
        status->setViewLink(s.chip);
        root->addWidget(topRow);

        // Profile tabs over the Logs / Connections / Graph tabs, as before.
        ui->verticalLayout_3->removeWidget(ui->splitter);
        root->addWidget(ui->splitter, 1);
        // Takes the splitter's room in the minimal view, so the row keeps its height.
        auto *filler = new QWidget(central);
        filler->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        filler->hide();
        s.filler = filler;
        root->addWidget(filler, 1);

        // Bottom bar: version | blocking badge | region · provider · network (dev) on the left,
        // the routing switch and, in Продвинутый, the traffic speed on the right.
        auto *bottomBar = makeCard(central, "slBottomBar");
        s.bottomBar = bottomBar;
        bottomBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *barLayout = new QHBoxLayout(bottomBar);
        barLayout->setContentsMargins(14, 6, 8, 6);
        barLayout->setSpacing(10);
        const auto separator = [bottomBar] {
            auto *line = new QFrame(bottomBar);
            line->setObjectName(QStringLiteral("slSep"));
            line->setFixedSize(1, 14);
            return line;
        };
        Corner &corners = makeCorners(status);
        status->setStopwatch(corners.stopwatch);
        barLayout->addWidget(corners.version);
        if (QWidget *badge = Shadowlos::Journal::MakeBlockingBadge(bottomBar)) {
            barLayout->addWidget(separator());
            barLayout->addWidget(badge, 0, Qt::AlignVCenter);
        }
        barLayout->addStretch(1);
        barLayout->addWidget(Shadowlos::Routing::MakeSwitch(bottomBar));
        s.speedSep = separator();
        barLayout->addWidget(s.speedSep);
        // The status labels: running and inbound stay (hidden) for upstream's code; the speed
        // shows in Продвинутый.
        for (QLabel *label : {ui->label_running, ui->label_inbound, ui->label_speed}) {
            ui->horizontalLayout->removeWidget(label);
            label->setProperty("slRole", "status");
            label->setWordWrap(false);
            repolish(label);
        }
        ui->label_running->setParent(central);
        ui->label_running->hide();
        ui->label_inbound->setParent(central);
        ui->label_inbound->hide();
        // As wide as a typical reading, so the window does not grow while the numbers change.
        ui->label_speed->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        barLayout->addWidget(ui->label_speed);
        root->addWidget(bottomBar);

        wideWatch() = new WideWatch(central);

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

        // After MainWindow's constructor, which sets these up later than this runs (and
        // restores the saved window size).
        QTimer::singleShot(0, window, [window, ui] {
            // MainWindow sizes the start button to the menu buttons' height; the views size it.
            ui->toolButton_program->removeEventFilter(window);

            if (auto *filter = qobject_cast<QToolButton *>(ui->tabWidget->cornerWidget(Qt::TopRightCorner))) {
                filter->setObjectName(QStringLiteral("slFilterButton"));
                filter->setIcon(icon("filter"));
                filter->setIconSize(QSize(16, 16));
                filter->setFixedSize(28, 28);
                filter->setCursor(Qt::PointingHandCursor);
                repolish(filter);
            }

            Shadowlos::ImportStaticProfilesIfEmpty();
            new ColumnFitter(ui);
            // Dev screenshots may ask for a view without saving it.
            View view = shell().view;
            if (demoHas("minimal")) view = View::Minimal;
            else if (demoHas("normal")) view = View::Normal;
            else if (demoHas("advanced")) view = View::Advanced;
            applyView(view, false);
            installStartupLatencyTest(window, ui);
            installLogPause(ui);
            if (demoHas("max")) {
                // The primary screen's whole work area, once the test has put the window on screen
                // (moving a minimized window to another screen spoils its restore).
                auto *poll = new QTimer(window);
                poll->setInterval(200);
                QObject::connect(poll, &QTimer::timeout, window, [window, poll] {
                    if (!window->isVisible() || window->isMinimized()) return;
                    poll->stop();
                    poll->deleteLater();
                    QScreen *screen = QGuiApplication::primaryScreen();
                    if (screen == nullptr) return;
                    const QRect frame = window->frameGeometry(), inner = window->geometry();
                    const QMargins edge(inner.left() - frame.left(), inner.top() - frame.top(),
                                        frame.right() - inner.right(), frame.bottom() - inner.bottom());
                    window->setGeometry(screen->availableGeometry().marginsRemoved(edge));
                });
                poll->start();
            } else if (demoHas("min")) {
                QTimer::singleShot(800, window, [window] { window->resize(window->minimumSize()); });
            }
            installDemoPopups(window, ui);

            applyCaptionToVisibleWindows();
        });
    }

} // namespace Shadowlos::Chrome
