#include "include/ui/shadowlos/ShadowlosFitScreen.hpp"

#include <QCursor>
#include <QDialog>
#include <QBoxLayout>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLayout>
#include <QMessageBox>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace {
    constexpr auto kWrapped = "slFitScreenWrapped";

    // The title bar and the frame do not exist yet when a dialog is about to be shown, so
    // leave room for them (Windows 11 at 100%: 31 px title bar plus 1 px borders; more at 125%+).
    constexpr int kFrameWidth = 16;
    constexpr int kFrameHeight = 48;

    QScreen *targetScreen(QWidget *window) {
        if (QWidget *parent = window->parentWidget()) {
            if (QWidget *top = parent->window(); top->isVisible()) return top->screen();
        }
        if (QScreen *screen = QGuiApplication::screenAt(QCursor::pos())) return screen;
        return window->screen();
    }

    struct ButtonRow {
        QDialogButtonBox *box = nullptr;
        QLayout *holder = nullptr; // the layout the box is a direct item of
    };

    // The dialog's OK / Cancel box: the one nearest the top of its layout tree, so a box deep
    // inside some page is never taken for it.
    ButtonRow findButtonRow(QLayout *top) {
        QList<QLayout *> queue{top};
        while (!queue.isEmpty()) {
            QLayout *layout = queue.takeFirst();
            for (int i = 0; i < layout->count(); ++i) {
                QLayoutItem *item = layout->itemAt(i);
                if (auto *box = qobject_cast<QDialogButtonBox *>(item->widget())) return {box, layout};
                if (QLayout *sub = item->layout()) queue << sub;
                else if (QWidget *w = item->widget(); w != nullptr && w->layout() != nullptr) queue << w->layout();
            }
        }
        return {};
    }

    // Takes the OK / Cancel row out of the dialog's layout and returns it. A box that shares a
    // horizontal row with other buttons (such as «Troubleshooting») takes the whole row along.
    QLayoutItem *takeButtonRow(QLayout *top) {
        const ButtonRow row = findButtonRow(top);
        if (row.box == nullptr) return nullptr;
        auto *rowLayout = qobject_cast<QBoxLayout *>(row.holder);
        auto *rowParent = qobject_cast<QLayout *>(row.holder->parent());
        if (row.holder != top && rowLayout != nullptr && rowParent != nullptr &&
            (rowLayout->direction() == QBoxLayout::LeftToRight || rowLayout->direction() == QBoxLayout::RightToLeft)) {
            rowParent->removeItem(rowLayout);
            rowLayout->setParent(nullptr);
            return rowLayout;
        }
        row.holder->removeWidget(row.box);
        return new QWidgetItem(row.box);
    }

    QRect freeArea(QDialog *dialog) {
        QScreen *screen = targetScreen(dialog);
        return screen != nullptr ? screen->availableGeometry() : QRect();
    }

    QSize roomIn(const QRect &free) {
        return {free.width() - kFrameWidth, free.height() - kFrameHeight};
    }

    // Width that shows the scroll area's contents without a horizontal bar.
    int scrolledWidth(QScrollArea *scroll) {
        const QWidget *content = scroll->widget();
        const int bar = scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll);
        return content->sizeHint().expandedTo(content->minimumSizeHint()).width() + bar;
    }

    // Resizes the dialog to [want] (already within the screen) and keeps it all on the screen,
    // centred on the window it belongs to when [centre] is set.
    void place(QDialog *dialog, const QSize &want, const QRect &free, bool centre) {
        QPoint middle = dialog->frameGeometry().center();
        dialog->setMinimumSize(dialog->minimumSize().boundedTo(want));
        dialog->resize(want);
        if (centre) {
            middle = free.center();
            if (QWidget *parent = dialog->parentWidget(); parent != nullptr && parent->window()->isVisible()) {
                middle = parent->window()->frameGeometry().center();
            }
        }
        const int x = qBound(free.left(), middle.x() - want.width() / 2, free.right() - want.width() - kFrameWidth);
        const int y = qBound(free.top(), middle.y() - want.height() / 2, free.bottom() - want.height() - kFrameHeight);
        dialog->move(x, y);
    }

    // Some dialogs fill in their contents after they are shown (the profile editor adds the
    // protocol's fields and the TLS column): widen to them, up to the screen's width.
    class WidenOnGrow final : public QObject {
    public:
        WidenOnGrow(QDialog *dialog, QScrollArea *scroll) : QObject(scroll), m_dialog(dialog), m_scroll(scroll) {
            scroll->widget()->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::LayoutRequest && !m_queued) {
                m_queued = true;
                QTimer::singleShot(0, this, [this] {
                    m_queued = false;
                    const QRect free = freeArea(m_dialog);
                    const int width = qMin(scrolledWidth(m_scroll), roomIn(free).width());
                    if (free.isEmpty() || width <= m_dialog->width()) return;
                    place(m_dialog, {width, m_dialog->height()}, free, false);
                });
            }
            return false;
        }

    private:
        QDialog *m_dialog;
        QScrollArea *m_scroll;
        bool m_queued = false;
    };

    // Moves the dialog's contents into a scroll area, keeping the OK / Cancel row under it.
    // Returns the width the dialog needs to show the contents without a horizontal bar.
    int wrapInScrollArea(QDialog *dialog, QLayout *layout) {
        const QMargins margins = layout->contentsMargins();
        const int spacing = qMax(layout->spacing(), 0);
        QLayoutItem *buttons = takeButtonRow(layout);

        auto *content = new QWidget;
        content->setObjectName("slFitScreenContent");
        // QWidget::setLayout takes a layout over from the widget that held it, with its widgets.
        content->setLayout(layout);

        auto *scroll = new QScrollArea(dialog);
        scroll->setObjectName("slFitScreenScroll");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(content);

        auto *outer = new QVBoxLayout(dialog);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        outer->addWidget(scroll, 1);
        if (buttons != nullptr) {
            auto *row = new QVBoxLayout;
            row->setContentsMargins(margins.left(), spacing, margins.right(), margins.bottom());
            if (QLayout *l = buttons->layout()) {
                row->addLayout(l);
            } else {
                row->addWidget(buttons->widget());
                delete buttons;
            }
            outer->addLayout(row);
        }

        dialog->setProperty(kWrapped, true);
        new WidenOnGrow(dialog, scroll);
        return scrolledWidth(scroll);
    }
}

namespace Shadowlos::FitScreen {
    void BeforeShow(QWidget *window) {
        auto *dialog = qobject_cast<QDialog *>(window);
        if (dialog == nullptr || qobject_cast<QMessageBox *>(dialog) != nullptr ||
            qobject_cast<QFileDialog *>(dialog) != nullptr) {
            return;
        }
        const QRect free = freeArea(dialog);
        const QSize room = roomIn(free);
        if (room.width() <= 0 || room.height() <= 0) return;

        const QSize size = dialog->size();
        const QSize minimum = dialog->minimumSizeHint().expandedTo(dialog->minimumSize());
        const bool tooBig = size.width() > room.width() || size.height() > room.height();
        const bool cannotShrink = minimum.width() > room.width() || minimum.height() > room.height();
        if (!tooBig && !cannotShrink) return;

        QSize want = size;
        if (QLayout *layout = dialog->layout();
            cannotShrink && layout != nullptr && !dialog->property(kWrapped).toBool()) {
            want.setWidth(qMax(want.width(), wrapInScrollArea(dialog, layout)));
        }
        place(dialog, want.boundedTo(room), free, true);
    }
}
