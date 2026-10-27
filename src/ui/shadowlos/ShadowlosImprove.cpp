#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#if SHADOWLOS_DEV

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

// «Улучшение обхода блокировок», the Android app's page of that name (shadowlos
// app/lib/ui/block_help_screen.dart on feat/app-v2-frame) on what the desktop has: the switch
// is the journal consent (the Tools menu action, shadowlos_send_journal), «Отправить логи» is a
// manual report, «Проверить доступность» runs the census now. Dev builds only: prod has no
// journal to send, so neither the page nor its menu item.
namespace Shadowlos::Journal {

    namespace {

        QLabel *makeLabel(QWidget *parent, const char *role, const QString &text = {}) {
            auto *label = new QLabel(text, parent);
            label->setProperty("slRole", role);
            label->setWordWrap(true);
            return label;
        }

        QFrame *makeCard(QWidget *parent) {
            auto *card = new QFrame(parent);
            card->setObjectName(QStringLiteral("slCard"));
            return card;
        }

        void setError(QLabel *label, bool error) {
            label->setProperty("slError", error);
            label->style()->unpolish(label);
            label->style()->polish(label);
        }

        QString consentHint(bool on) {
            // The off line is the Android page's; the on line says what the desktop sends.
            return on ? QStringLiteral("Включено — отправляем журнал ошибок и результаты проверки сайтов.")
                      : QStringLiteral("Выключено — мы ничего не отправляем.");
        }

        QString sendText(ManualResult result, int status) {
            switch (result) {
                case ManualResult::Sent: return QStringLiteral("Логи отправлены. Спасибо!");
                case ManualResult::Off: return QStringLiteral("Отправка выключена: включите переключатель выше.");
                case ManualResult::NoToken: return QStringLiteral("Не удалось отправить: нет подписки Shadowlos.");
                case ManualResult::NoConnection: return QStringLiteral("Нет связи с сервером. Попробуйте позже.");
                case ManualResult::TooOften: return QStringLiteral("Слишком часто. Попробуйте через минуту.");
                default: return QStringLiteral("Сервер не принял логи (код %1).").arg(status);
            }
        }

        class ImproveDialog : public QDialog {
        public:
            explicit ImproveDialog(QWidget *parent) : QDialog(parent) {
                setObjectName(QStringLiteral("slImproveDialog"));
                setWindowTitle(QStringLiteral("Улучшение обхода блокировок"));
                setAttribute(Qt::WA_DeleteOnClose);
                setMinimumWidth(480);

                auto *root = new QVBoxLayout(this);
                root->setContentsMargins(16, 16, 16, 16);
                root->setSpacing(10);

                root->addWidget(makeLabel(this, "improveHeading", QStringLiteral("Участие")));

                // The switch: the Tools menu action is the one source, both follow it.
                auto *switchCard = makeCard(this);
                auto *switchRow = new QHBoxLayout(switchCard);
                switchRow->setContentsMargins(16, 12, 16, 12);
                switchRow->setSpacing(12);
                auto *switchLabel = makeLabel(switchCard, "improveSwitch", QStringLiteral("Отправлять журнал разработчикам"));
                m_consent = new QCheckBox(switchCard);
                m_consent->setObjectName(QStringLiteral("slImproveConsent"));
                m_consent->setAccessibleName(switchLabel->text());
                m_consent->setProperty("slRole", "toggle");
                m_consent->setCursor(Qt::PointingHandCursor);
                switchRow->addWidget(switchLabel, 1);
                switchRow->addWidget(m_consent);
                root->addWidget(switchCard);
                m_hint = makeLabel(this, "improveHint");
                root->addWidget(m_hint);

                QAction *action = ConsentAction();
                const bool on = action != nullptr && action->isChecked();
                m_consent->setChecked(on);
                m_consent->setEnabled(action != nullptr);
                m_hint->setText(consentHint(on));
                if (action != nullptr) {
                    connect(m_consent, &QCheckBox::toggled, action, &QAction::setChecked);
                    connect(action, &QAction::toggled, this, [this](bool v) {
                        m_consent->setChecked(v);
                        m_hint->setText(consentHint(v));
                    });
                }

                // The two buttons, as on Android: send on the left, the check (primary) on the right.
                auto *buttons = new QHBoxLayout();
                buttons->setSpacing(12);
                m_send = new QPushButton(QStringLiteral("Отправить логи"), this);
                m_send->setObjectName(QStringLiteral("slImproveSend"));
                m_send->setAutoDefault(false);
                m_check = new QPushButton(QStringLiteral("Проверить доступность"), this);
                m_check->setObjectName(QStringLiteral("slImproveCheck"));
                m_check->setDefault(true);
                for (auto *b : {m_send, m_check}) {
                    b->setMinimumHeight(40);
                    b->setCursor(Qt::PointingHandCursor);
                    buttons->addWidget(b, 1);
                }
                root->addLayout(buttons);
                m_sendStatus = makeLabel(this, "improveStatus");
                m_sendStatus->setObjectName(QStringLiteral("slImproveSendStatus"));
                m_sendStatus->hide();
                root->addWidget(m_sendStatus);

                // The result of the last check (the Android «Результат проверки» card).
                m_result = makeCard(this);
                auto *resultLayout = new QVBoxLayout(m_result);
                resultLayout->setContentsMargins(16, 12, 16, 12);
                resultLayout->setSpacing(6);
                auto *resultTitle = makeLabel(m_result, "improveCardTitle", QStringLiteral("Результат проверки"));
                m_when = makeLabel(m_result, "improveHint");
                m_verdict = makeLabel(m_result, "improveVerdict");
                m_verdict->setObjectName(QStringLiteral("slImproveVerdict"));
                m_counts = makeLabel(m_result, "improveText");
                m_place = makeLabel(m_result, "improveText");
                for (auto *w : {resultTitle, m_when, m_verdict, m_counts, m_place}) resultLayout->addWidget(w);
                m_result->hide();
                root->addWidget(m_result);

                // What is sent and what for («Что мы отправляем» on Android).
                auto *info = makeCard(this);
                auto *infoLayout = new QVBoxLayout(info);
                infoLayout->setContentsMargins(16, 12, 16, 12);
                infoLayout->setSpacing(8);
                infoLayout->addWidget(makeLabel(info, "improveCardTitle", QStringLiteral("Что мы отправляем")));
                infoLayout->addWidget(makeLabel(info, "improveText", QStringLiteral(
                    "Мы проверяем, какие сайты открываются у вас без VPN, и отправляем журнал ошибок приложения. "
                    "По этим данным мы собираем «Настройку под ваш регион» — для вашего региона и оператора. "
                    "Личные данные не отправляем: ни имени, ни Telegram, ни IP-адреса.")));
                root->addWidget(info);

                root->addStretch(1);
                auto *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
                if (auto *close = box->button(QDialogButtonBox::Close)) {
                    close->setText(QStringLiteral("Закрыть"));
                    close->setAutoDefault(false);
                }
                connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
                root->addWidget(box);

                connect(m_send, &QPushButton::clicked, this, [this] { send(); });
                connect(m_check, &QPushButton::clicked, this, [this] { check(); });

                CensusNow last;
                if (CensusLast(&last)) showResult(last);
            }

        private:
            void send() {
                m_send->setEnabled(false);
                m_send->setText(QStringLiteral("Отправляем…"));
                m_sendStatus->hide();
                QPointer<ImproveDialog> self(this);
                SendManualReport([self](ManualResult result, int status) {
                    if (!self) return;
                    self->m_send->setEnabled(true);
                    self->m_send->setText(QStringLiteral("Отправить логи"));
                    self->m_sendStatus->setText(sendText(result, status));
                    setError(self->m_sendStatus, result != ManualResult::Sent);
                    self->m_sendStatus->show();
                });
            }

            void check() {
                m_check->setEnabled(false);
                m_check->setText(QStringLiteral("Проверяем…"));
                QPointer<ImproveDialog> self(this);
                CensusMeasureNow([self](const CensusNow &r) {
                    if (!self) return;
                    self->m_check->setEnabled(true);
                    self->m_check->setText(QStringLiteral("Проверить доступность"));
                    self->showResult(r);
                });
            }

            void showResult(const CensusNow &r) {
                m_result->show();
                if (!r.measured) {
                    m_when->hide();
                    m_verdict->setText(QStringLiteral("Проверка не прошла"));
                    m_counts->setText(r.note);
                    m_place->hide();
                    return;
                }
                m_when->setText(QStringLiteral("Проверено · ") + QDateTime::fromMSecsSinceEpoch(r.at).toString(QStringLiteral("HH:mm")));
                m_when->show();
                m_verdict->setText(r.verdict);
                m_counts->setText(QStringLiteral("Открылись напрямую: ") + r.counts);
                if (r.region.isEmpty() && r.op.isEmpty()) {
                    m_place->hide();
                } else {
                    m_place->setText(QStringLiteral("Регион: ") + (r.region.isEmpty() ? QStringLiteral("?") : r.region) +
                                     QStringLiteral(", оператор: ") + (r.op.isEmpty() ? QStringLiteral("?") : r.op));
                    m_place->show();
                }
            }

            QCheckBox *m_consent = nullptr;
            QLabel *m_hint = nullptr;
            QPushButton *m_send = nullptr;
            QPushButton *m_check = nullptr;
            QLabel *m_sendStatus = nullptr;
            QFrame *m_result = nullptr;
            QLabel *m_when = nullptr;
            QLabel *m_verdict = nullptr;
            QLabel *m_counts = nullptr;
            QLabel *m_place = nullptr;
        };

        QPointer<ImproveDialog> openDialog;
        QPointer<QWidget> mainWindow;

        // One page at a time. A window of its own (no owner), like a page of the app: it has
        // its own taskbar entry, and it goes when the main window goes.
        void openPage() {
            if (!openDialog) {
                openDialog = new ImproveDialog(nullptr);
                if (mainWindow) {
                    openDialog->setWindowIcon(mainWindow->windowIcon());
                    QObject::connect(mainWindow, &QObject::destroyed, openDialog, &QObject::deleteLater);
                    if (mainWindow->isVisible() && !mainWindow->isMinimized()) {
                        openDialog->adjustSize();
                        openDialog->move(mainWindow->geometry().center() - openDialog->rect().center());
                    }
                }
                openDialog->show();
            }
            openDialog->raise();
            openDialog->activateWindow();
        }

    } // namespace

    void InstallImprovePage(QWidget *window, QMenu *menu) {
        mainWindow = window;
        auto *action = new QAction(QStringLiteral("Улучшение обхода блокировок"), menu);
        action->setObjectName(QStringLiteral("slImproveAction"));
        QObject::connect(action, &QAction::triggered, window, [] { openPage(); });
        menu->addSeparator();
        menu->addAction(action);
    }

    QWidget *MakeImproveLink(QWidget *parent) {
        auto *link = new QToolButton(parent);
        link->setObjectName(QStringLiteral("slImproveLink"));
        link->setText(QStringLiteral("Улучшение обхода блокировок"));
        link->setToolTip(QStringLiteral("Проверить доступность сайтов и отправить логи разработчикам"));
        link->setProperty("slRole", "link");
        link->setCursor(Qt::PointingHandCursor);
        link->setAutoRaise(true);
        QObject::connect(link, &QToolButton::clicked, link, [] { openPage(); });
        return link;
    }

} // namespace Shadowlos::Journal

#endif // SHADOWLOS_DEV
