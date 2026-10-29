#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"

#include <QAction>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// The first-launch consent window (lead 0545Z, 0550Z, 0555Z; text: team/mockups/consent/
// consent-text.html, word for word). Both builds.
//
// When: only in Обычный or Продвинутый, never in Минимальный; once per app version until the
// user says «Да» (then never again). «Нет» or no answer is asked again after an update, once.
// Closing it without an answer (Esc) is no answer: asked again at the next launch.
// The answer sets the «Улучшение обхода» switch; turning the switch on later counts as «Да».
// Until a «Да», ConsentOn() is false: nothing is collected or sent. While the window is open,
// geo and the census may look (ConsentPreview) so it can show what the user gets; that sends
// nothing to our telemetry.
namespace Shadowlos::Journal {

    namespace {

        // ── the rules (pure) ─────────────────────────────────────────────────

        constexpr int kNever = 0, kYes = 1, kNo = 2;

        // Whether to show the window now.
        bool shouldAsk(int consent, const QString &answeredVersion, const QString &version, bool fullView, bool askedThisRun) {
            return fullView && !askedThisRun && consent != kYes && answeredVersion != version;
        }

        // What counts as consent for collecting and sending (ShadowlosTelemetry.cpp ConsentOn).
        bool consentCounts(int consent, int switchOn) {
            return consent == kYes && switchOn != 0;
        }

        struct Stored {
            int consent = kNever;
            int switchOn = 0;
            QString version;
        };

        void answer(Stored &s, bool yes, const QString &version) {
            s.consent = yes ? kYes : kNo;
            s.switchOn = yes ? 1 : 0;
            s.version = version;
        }

        // The switch turned on in the bottom bar (or Продвинутый's toggle): «Да» from now on.
        void switchedOn(Stored &s) {
            s.switchOn = 1;
            s.consent = kYes;
        }

        // ── the window ───────────────────────────────────────────────────────

        const char *kSheet = R"(
            QFrame#slConsentCard { background: #1c2128; border: 1px solid #2c3440; border-radius: 16px; }
            QLabel { color: #b4bcc6; font-size: 14px; background: transparent; }
            QLabel#slConsentTitle { color: #e8eaed; font-size: 21px; font-weight: 600; }
            QFrame#slConsentGets, QFrame#slConsentBox { background: #161b21; border: 1px solid #262e38; border-radius: 12px; }
            QLabel#slConsentGetsTitle { color: #8a929c; font-size: 12.5px; }
            QLabel[slRole="name"] { color: #e8eaed; font-weight: 500; }
            QLabel[slRole="value"] { color: #8a929c; }
            QLabel#slConsentDot { border-radius: 4px; background: transparent; }
            QLabel#slConsentDot[slLevel="ok"] { background: #3ccf6e; }
            QLabel#slConsentDot[slLevel="warn"] { background: #e3b341; }
            QLabel#slConsentDot[slLevel="bad"] { background: #e5534b; }
            QPushButton#slConsentMore { color: #7fb2ff; font-size: 13.5px; border: none; background: transparent; text-align: left; padding: 2px 0; }
            QPushButton#slConsentNo, QPushButton#slConsentYes { border: none; border-radius: 10px; padding: 11px 0; font-weight: 600; font-size: 14.5px; }
            QPushButton#slConsentNo { background: #262d36; color: #cfd3d8; }
            QPushButton#slConsentNo:hover { background: #2e3640; }
            QPushButton#slConsentYes { background: #2f8cff; color: #ffffff; }
            QPushButton#slConsentYes:hover { background: #4198ff; }
            QLabel#slConsentNote { color: #7d8590; font-size: 12px; }
        )";

        // consent-text.html, the box under «Какие данные мы собираем».
        QString boxHtml() {
            const QString li = QStringLiteral("<li style=\"margin-bottom:6px\"><span style=\"color:#e8eaed\">%1</span>%2</li>");
            const auto ex = [](const char *text) {
                return QStringLiteral("<br><span style=\"color:#8a929c;font-size:12.5px\">") + QString::fromUtf8(text) + QStringLiteral("</span>");
            };
            const QString ul = QStringLiteral("<ul style=\"margin:0 0 6px 0;-qt-list-indent:1\">");
            const QString h3 = QStringLiteral("<p style=\"color:#e8eaed;font-weight:600;margin:4px 0\">%1</p>");
            QString html = QStringLiteral("<div style=\"font-size:13px;color:#b4bcc6\">");
            html += h3.arg(QStringLiteral("Что отправляется и зачем")) + ul;
            html += li.arg(QStringLiteral("Сервисы из списка режима «Удобный» (сайты и приложения), которые у вас не открылись."),
                           ex("Проверяем, не блокирует ли их само включение VPN, и настраиваем по ним режим «Удобный»."));
            html += li.arg(QStringLiteral("Регион, провайдер, тип сети и тип блокировок в вашей сети."),
                           ex("Блокировки отличаются по регионам и провайдерам, так мы подстраиваем обход под ваш регион."));
            html += li.arg(QStringLiteral("Ошибки приложения: сбои и неудачные подключения."),
                           ex("Находим, что мешает подключению, и улучшаем приложение."));
            html += QStringLiteral("</ul>") + h3.arg(QStringLiteral("Что не отправляется")) + ul;
            html += QStringLiteral("<li style=\"margin-bottom:6px\">Ваш IP‑адрес</li>");
            html += QStringLiteral("<li style=\"margin-bottom:6px\">Какими сайтами и приложениями вы пользуетесь и что в них делаете</li>");
            html += QStringLiteral("<li style=\"margin-bottom:6px\">Имя, контакты, переписка и другие личные данные") +
                    ex("У нас и так нет к этому доступа: всё зашифровано. Пишем на всякий случай, чтобы вы не волновались.") +
                    QStringLiteral("</li></ul>");
            html += QStringLiteral("<p style=\"color:#8a929c;font-size:12.5px;margin:0\">Данные отправляются анонимно, раз в день.</p></div>");
            return html;
        }

        // Dims the main window while the window is open, and follows its size.
        class Dim : public QWidget {
        public:
            explicit Dim(QWidget *window) : QWidget(window) {
                setAttribute(Qt::WA_TransparentForMouseEvents, false);
                setStyleSheet(QStringLiteral("background: rgba(0, 0, 0, 150);"));
                setAttribute(Qt::WA_StyledBackground, true);
                setGeometry(window->rect());
                window->installEventFilter(this);
                raise();
                show();
            }

        protected:
            bool eventFilter(QObject *obj, QEvent *event) override {
                if (event->type() == QEvent::Resize && obj == parentWidget()) setGeometry(parentWidget()->rect());
                return false;
            }
        };

        class ConsentDialog : public QDialog {
        public:
            ConsentDialog(QWidget *window, std::function<void(bool yes)> answered)
                : QDialog(window, Qt::Dialog | Qt::FramelessWindowHint), answered_(std::move(answered)) {
                setAttribute(Qt::WA_TranslucentBackground, true);
                setAttribute(Qt::WA_DeleteOnClose, true);
                setWindowModality(Qt::WindowModal);
                setWindowTitle(QStringLiteral("Помогите улучшить обход блокировок"));
                setStyleSheet(QLatin1String(kSheet));
                auto *outer = new QVBoxLayout(this);
                outer->setContentsMargins(0, 0, 0, 0);
                auto *card = new QFrame(this);
                card->setObjectName(QStringLiteral("slConsentCard"));
                card->setFixedWidth(520);
                outer->addWidget(card);
                auto *v = new QVBoxLayout(card);
                v->setContentsMargins(28, 26, 28, 22);
                v->setSpacing(0);

                auto *title = new QLabel(QStringLiteral("Помогите улучшить обход блокировок"), card);
                title->setObjectName(QStringLiteral("slConsentTitle"));
                v->addWidget(title);
                v->addSpacing(10);
                auto *lead = new QLabel(QStringLiteral("Разрешите отправлять анонимные данные о сервисах, которые у вас не открываются, и об ошибках. "
                                                       "По ним мы настраиваем режим «Удобный» и улучшаем обход для вашего региона."), card);
                lead->setWordWrap(true);
                v->addWidget(lead);
                v->addSpacing(16);

                auto *gets = new QFrame(card);
                gets->setObjectName(QStringLiteral("slConsentGets"));
                auto *g = new QVBoxLayout(gets);
                g->setContentsMargins(14, 12, 14, 12);
                g->setSpacing(8);
                auto *getsTitle = new QLabel(QStringLiteral("Взамен вы увидите в приложении:"), gets);
                getsTitle->setObjectName(QStringLiteral("slConsentGetsTitle"));
                g->addWidget(getsTitle);
                const auto row = [gets, g](const QString &name, QLabel **value, QLabel **dot) {
                    auto *line = new QHBoxLayout();
                    auto *n = new QLabel(name, gets);
                    n->setProperty("slRole", "name");
                    line->addWidget(n);
                    line->addStretch(1);
                    if (dot != nullptr) {
                        *dot = new QLabel(gets);
                        (*dot)->setObjectName(QStringLiteral("slConsentDot"));
                        (*dot)->setFixedSize(8, 8);
                        (*dot)->hide();
                        line->addWidget(*dot, 0, Qt::AlignVCenter);
                        line->addSpacing(6);
                    }
                    *value = new QLabel(gets);
                    (*value)->setProperty("slRole", "value");
                    line->addWidget(*value);
                    g->addLayout(line);
                };
                row(QStringLiteral("Ваш IP"), &ip_, nullptr);
                row(QStringLiteral("Регион и провайдер"), &region_, nullptr);
                row(QStringLiteral("Блокировки в вашей сети"), &blocking_, &dot_);
                v->addWidget(gets);
                v->addSpacing(14);

                more_ = new QPushButton(card);
                more_->setObjectName(QStringLiteral("slConsentMore"));
                more_->setAccessibleName(QStringLiteral("Какие данные мы собираем"));
                more_->setCursor(Qt::PointingHandCursor);
                more_->setAutoDefault(false);
                more_->setFocusPolicy(Qt::TabFocus);
                v->addWidget(more_, 0, Qt::AlignLeft);
                box_ = new QFrame(card);
                box_->setObjectName(QStringLiteral("slConsentBox"));
                auto *b = new QVBoxLayout(box_);
                b->setContentsMargins(14, 12, 14, 12);
                auto *text = new QLabel(boxHtml(), box_);
                text->setTextFormat(Qt::RichText);
                text->setWordWrap(true);
                b->addWidget(text);
                box_->hide();
                v->addSpacing(8);
                v->addWidget(box_);
                setMore(false);
                connect(more_, &QPushButton::clicked, this, [this] { setMore(box_->isHidden()); });

                v->addSpacing(18);
                auto *buttons = new QHBoxLayout();
                buttons->setSpacing(10);
                auto *no = new QPushButton(QStringLiteral("Нет"), card);
                no->setObjectName(QStringLiteral("slConsentNo"));
                no->setAutoDefault(false);
                no->setCursor(Qt::PointingHandCursor);
                auto *yes = new QPushButton(QStringLiteral("Да, разрешить"), card);
                yes->setObjectName(QStringLiteral("slConsentYes"));
                yes->setDefault(true);
                yes->setAutoDefault(true);
                yes->setCursor(Qt::PointingHandCursor);
                buttons->addWidget(no, 1);
                buttons->addWidget(yes, 1);
                v->addLayout(buttons);
                v->addSpacing(10);
                auto *note = new QLabel(QStringLiteral("Можно изменить позже в «Улучшение обхода»"), card);
                note->setObjectName(QStringLiteral("slConsentNote"));
                note->setAlignment(Qt::AlignHCenter);
                v->addWidget(note);
                connect(no, &QPushButton::clicked, this, [this] { finish(false); });
                connect(yes, &QPushButton::clicked, this, [this] { finish(true); });
                yes->setFocus();

                refresh();
                auto *timer = new QTimer(this);
                timer->setInterval(1000);
                connect(timer, &QTimer::timeout, this, [this] { refresh(); });
                timer->start();
            }

            // Centred over the main window, kept on its screen.
            void place() {
                adjustSize();
                QWidget *w = parentWidget();
                if (w == nullptr) return;
                QRect r(QPoint(0, 0), size());
                r.moveCenter(w->frameGeometry().center());
                if (const QScreen *screen = w->screen()) {
                    const QRect area = screen->availableGeometry();
                    r.moveLeft(qBound(area.left(), r.left(), qMax(area.left(), area.right() - r.width())));
                    r.moveTop(qBound(area.top(), r.top(), qMax(area.top(), area.bottom() - r.height())));
                }
                move(r.topLeft());
            }

        private:
            // Esc closes it without an answer (asked again next launch); Enter is «Да», the default.
            void setMore(bool open) {
                more_->setText(QStringLiteral("Какие данные мы собираем ") + (open ? QStringLiteral("⌃") : QStringLiteral("⌄")));
                box_->setVisible(open);
                place();
            }

            // What the user gets, from what geo and the census know now.
            void refresh() {
                const QString unknown = QStringLiteral("определяем…");
                const QString ip = LocalIpMasked();
                ip_->setText(ip.isEmpty() ? unknown : ip);
                const QString where = GeoRegionProvider();
                region_->setText(where.isEmpty() ? unknown : where);
                QString level;
                const QString verdict = CensusVerdictNow(&level);
                blocking_->setText(verdict.isEmpty() ? unknown : verdict);
                dot_->setVisible(!verdict.isEmpty());
                if (dot_->property("slLevel").toString() != level) {
                    dot_->setProperty("slLevel", level);
                    dot_->style()->unpolish(dot_);
                    dot_->style()->polish(dot_);
                }
            }

            void finish(bool yes) {
                const auto done = std::move(answered_);
                answered_ = nullptr;
                if (done) done(yes);
                accept();
            }

            std::function<void(bool)> answered_;
            QLabel *ip_ = nullptr, *region_ = nullptr, *blocking_ = nullptr, *dot_ = nullptr;
            QPushButton *more_ = nullptr;
            QFrame *box_ = nullptr;
        };

        // ── state (UI thread) ────────────────────────────────────────────────

        struct State {
            QPointer<QWidget> window;
            QPointer<QAction> consent;
            QPointer<ConsentDialog> dialog;
            QPointer<QTimer> waitShown;
            bool fullView = false;
            bool askedThisRun = false;
        };

        State &st() {
            static State s;
            return s;
        }

        Configs::SettingsRepo *settings() {
            return Configs::dataManager->settingsRepo.get();
        }

        bool askNow() {
            const auto *set = settings();
            return shouldAsk(set->shadowlos_consent, set->shadowlos_consent_version, QStringLiteral(NKR_VERSION), st().fullView,
                             st().askedThisRun);
        }

        bool windowShown() {
            QWidget *w = st().window;
            return w != nullptr && w->isVisible() && !w->isMinimized();
        }

        void show() {
            auto &s = st();
            if (s.dialog || !askNow() || !windowShown()) return;
            s.askedThisRun = true;
            QWidget *w = s.window;
            auto *dim = new Dim(w);
            auto *dialog = new ConsentDialog(w, [](bool yes) {
                auto *set = settings();
                Stored stored{set->shadowlos_consent, set->shadowlos_send_journal, set->shadowlos_consent_version};
                answer(stored, yes, QStringLiteral(NKR_VERSION));
                set->shadowlos_consent = stored.consent;
                set->shadowlos_consent_version = stored.version;
                if (!yes) set->shadowlos_send_journal = 0;
                set->Save();
                LOG_INFO(QStringLiteral("[consent] answered %1 for %2").arg(yes ? "yes" : "no", QStringLiteral(NKR_VERSION)));
                // The switch follows (its handler saves the switch and starts the summary).
                if (QAction *a = st().consent) a->setChecked(yes);
            });
            QObject::connect(dialog, &QObject::destroyed, dim, [dim] { dim->deleteLater(); });
            QObject::connect(dialog, &QDialog::rejected, dialog, [] { LOG_INFO(QStringLiteral("[consent] closed without an answer")); });
            s.dialog = dialog;
            LOG_INFO(QStringLiteral("[consent] window shown"));
            dialog->place();
            dialog->show();
            // Whereami now, so the IP and region rows fill in while the user reads.
            GeoKick();
        }

        // The main window may be minimized or in the tray at start: wait until it is on screen.
        void showWhenShown() {
            auto &s = st();
            if (windowShown()) {
                QTimer::singleShot(700, s.window, [] { show(); });
                return;
            }
            if (s.waitShown) return;
            auto *timer = new QTimer(s.window);
            timer->setInterval(1000);
            QObject::connect(timer, &QTimer::timeout, timer, [timer] {
                if (!askNow()) {
                    timer->deleteLater();
                    return;
                }
                if (!windowShown()) return;
                timer->deleteLater();
                QTimer::singleShot(700, st().window, [] { show(); });
            });
            timer->start();
            s.waitShown = timer;
        }

    } // namespace

    void InstallConsent(QWidget *window, QAction *consent) {
        st().window = window;
        st().consent = consent;
    }

    void ConsentViewChanged(bool fullView) {
        st().fullView = fullView;
        if (st().window && askNow()) showWhenShown();
    }

    bool ConsentPreview() {
        return !st().dialog.isNull();
    }

    QString ConsentSelfTest() {
        QStringList bad;
        const auto expect = [&bad](const char *name, bool ok) {
            if (!ok) bad << QString::fromLatin1(name);
        };
        const QString v14 = QStringLiteral("1.0.14"), v15 = QStringLiteral("1.0.15");
        // Never in Минимальный; once on the first Обычный/Продвинутый.
        expect("never in Минимальный", !shouldAsk(kNever, {}, v14, false, false));
        expect("first Обычный/Продвинутый asks", shouldAsk(kNever, {}, v14, true, false));
        expect("once per run", !shouldAsk(kNever, {}, v14, true, true));
        expect("closed without an answer: asked at the next launch", shouldAsk(kNever, {}, v14, true, false));
        // «Да» is never asked again, not even after an update.
        Stored s;
        answer(s, true, v14);
        expect("«Да» stored", s.consent == kYes && s.switchOn == 1 && s.version == v14);
        expect("«Да» not asked again", !shouldAsk(s.consent, s.version, v14, true, false));
        expect("«Да» not asked after an update", !shouldAsk(s.consent, s.version, v15, true, false));
        // «Нет»: asked again on a new version, once per version.
        Stored n;
        answer(n, false, v14);
        expect("«Нет» stored, switch off", n.consent == kNo && n.switchOn == 0 && n.version == v14);
        expect("«Нет» not asked again in the same version", !shouldAsk(n.consent, n.version, v14, true, false));
        expect("«Нет» asked on a new version", shouldAsk(n.consent, n.version, v15, true, false));
        expect("«Нет» on a new version: never in Минимальный", !shouldAsk(n.consent, n.version, v15, false, false));
        answer(n, false, v15);
        expect("«Нет» asked only once per version", !shouldAsk(n.consent, n.version, v15, true, false));
        // The switch turned on since «Нет» is «Да».
        switchedOn(n);
        expect("switch on after «Нет» is «Да»", n.consent == kYes && !shouldAsk(n.consent, n.version, QStringLiteral("1.0.16"), true, false));
        // Nothing collected or sent before an answer or after «Нет».
        expect("nothing before an answer, even with the old switch default on", !consentCounts(kNever, 1));
        expect("nothing after «Нет»", !consentCounts(kNo, 0) && !consentCounts(kNo, 1));
        expect("«Да» with the switch on counts", consentCounts(kYes, 1));
        expect("«Да», then the switch off: nothing", !consentCounts(kYes, 0));
        return bad.join(QStringLiteral("; "));
    }

} // namespace Shadowlos::Journal
