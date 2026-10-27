#include "include/ui/shadowlos/ShadowlosUpdater.hpp"

#include "include/ui/mainwindow.h"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "include/ui/shadowlos/ShadowlosJournal.hpp"
#include "NkrVersion.h"

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#ifdef Q_OS_WIN
#include "3rdparty/WinCommander.hpp"
#include <windows.h>
#endif

namespace Shadowlos::Updater {

    namespace {

        constexpr int kFirstCheckMs = 30 * 1000;
        // Dev builds follow a channel that moves several times a day: an app left running
        // (closing the window only hides it to the tray) must not wait 6 h to see a release.
        constexpr int kCheckEveryMs = SHADOWLOS_DEV ? 20 * 60 * 1000 : 6 * 60 * 60 * 1000;

        QString translate(const char *text) {
            return QCoreApplication::translate("ShadowlosUpdater", text);
        }

        QString appDir() {
            return QCoreApplication::applicationDirPath();
        }

        bool dirWritable(const QString &dir) {
            QFile probe(dir + QStringLiteral("/.sl-write-probe"));
            if (!probe.open(QIODevice::WriteOnly)) return false;
            probe.close();
            probe.remove();
            return true;
        }

        QString localWorkDir() {
            return appDir() + QStringLiteral("/update");
        }

        QString userWorkDir() {
            return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                   QStringLiteral("/" SHADOWLOS_EXE_NAME "/update");
        }

        // Where this app downloads: update\ next to the exe, or %LOCALAPPDATA%\<exe name>\update
        // when the install folder is read-only (Program Files), where applying asks for UAC once.
        QString workDir() {
            static const QString dir = [] {
                const QString local = localWorkDir();
                if (QDir().mkpath(local) && dirWritable(local)) return local;
                QDir().mkpath(userWorkDir());
                return userWorkDir();
            }();
            return dir;
        }

        // The dir `selfupdate apply` left its pending marker in, if this start follows an
        // update. Both places are looked at: an elevated relaunch resolves workDir()
        // differently from the unelevated app that downloaded the update.
        QString pendingDir() {
            for (const QString &dir : {localWorkDir(), userWorkDir()}) {
                if (QFileInfo::exists(dir + QStringLiteral("/pending"))) return dir;
            }
            return {};
        }

        // The check running now, stopped before an apply moves its files.
        QPointer<QProcess> &currentCheck() {
            static QPointer<QProcess> proc;
            return proc;
        }

        // The channel is fixed by the build: dev only ever reads the dev channel.
        // SHADOWLOS_UPDATE_BASE points a build at a test server; the signature check
        // (public key compiled into ThroneCore) is the same either way.
        QString baseUrl() {
            const QString env = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SHADOWLOS_UPDATE_BASE"));
            if (!env.isEmpty()) return env;
            return SHADOWLOS_DEV ? QStringLiteral("https://master.shadowlos.xyz/api/desktop/app/dev")
                                 : QStringLiteral("https://master.shadowlos.xyz/api/desktop/app");
        }

        QString appId() {
            return SHADOWLOS_DEV ? QStringLiteral("shadowlos-throne-dev") : QStringLiteral("shadowlos-throne");
        }

        // The updater is a command of the core: ThroneCore.exe selfupdate ...
        QString coreExe() {
            return appDir() + QStringLiteral("/ThroneCore.exe");
        }

        // The short line right of «Обновить». It takes only the room the menu card has left,
        // so it never pushes the routing switch along. When that room is too small for the
        // sentence (a narrow window: the default size has none), the sentence hangs from the
        // button's lower edge instead, over the gap between the menu card and the cards,
        // as a small tag inside the window, not a pop-up.
        class StatusLine final : public QLabel {
        public:
            StatusLine(QWidget *parent, QWidget *window, QWidget *anchor)
                : QLabel(parent), m_anchor(anchor), m_hang(new QLabel(window)) {
                setObjectName(QStringLiteral("slUpdateStatus"));
                setProperty("slRole", "updateStatus");
                setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
                m_hang->setObjectName(QStringLiteral("slUpdateStatusTag"));
                m_hang->setProperty("slRole", "updateStatusTag");
                m_hang->setAttribute(Qt::WA_TransparentForMouseEvents);
                m_hang->hide();
                parent->installEventFilter(this);
            }

            void showText(const QString &text, bool error) {
                setText(text);
                setToolTip(text);
                m_hang->setText(text);
                for (QWidget *w : {static_cast<QWidget *>(this), static_cast<QWidget *>(m_hang)}) {
                    w->setProperty("slError", error);
                    w->style()->unpolish(w);
                    w->style()->polish(w);
                }
                rewant();
                update();
                // The layout settles first; then it is known whether the sentence fits.
                QTimer::singleShot(0, this, [this] { place(); });
            }

            // All of the sentence or nothing: only room that nothing else in the row wants is
            // taken, so the button keeps its label and the switch only moves into free space.
            QSize sizeHint() const override { return {m_want, fontMetrics().height() + 4}; }
            QSize minimumSizeHint() const override { return {0, fontMetrics().height() + 4}; }

        protected:
            void paintEvent(QPaintEvent *) override {
                if (!fits()) return;
                QPainter p(this);
                p.setPen(palette().color(QPalette::WindowText));
                p.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, text());
            }
            void resizeEvent(QResizeEvent *e) override {
                QLabel::resizeEvent(e);
                place();
            }
            void moveEvent(QMoveEvent *e) override {
                QLabel::moveEvent(e);
                place();
            }
            bool eventFilter(QObject *watched, QEvent *event) override {
                if (watched == parentWidget() && event->type() == QEvent::Resize) rewant();
                return false;
            }

        private:
            int fullWidth() const { return fontMetrics().horizontalAdvance(text()) + 4; }

            // The row's free width with this line left out: margins, the other items' size
            // hints and the gaps between them (a few px spare for rounding).
            int freeWidth() const {
                const auto *row = qobject_cast<QBoxLayout *>(parentWidget()->layout());
                if (row == nullptr) return 0;
                const QMargins m = row->contentsMargins();
                int used = m.left() + m.right() + 8;
                for (int i = 0; i < row->count(); ++i) {
                    QLayoutItem *item = row->itemAt(i);
                    if (item->widget() == this || item->isEmpty()) {
                        if (item->spacerItem()) used += item->sizeHint().width();
                        continue;
                    }
                    used += item->sizeHint().width() + row->spacing();
                }
                return parentWidget()->width() - used;
            }

            void rewant() {
                const int want = !text().isEmpty() && freeWidth() >= fullWidth() ? fullWidth() : 0;
                if (want == m_want) return;
                m_want = want;
                updateGeometry();
            }
            bool fits() const { return !text().isEmpty() && width() >= fullWidth(); }

            void place() {
                if (text().isEmpty() || fits() || m_anchor.isNull()) {
                    m_hang->hide();
                    return;
                }
                QWidget *window = m_hang->parentWidget();
                m_hang->adjustSize();
                const QPoint below = m_anchor->mapTo(window, QPoint(0, m_anchor->height()));
                // The anchor's card ends a few px below the button; the tag straddles that edge.
                const int maxWidth = std::max(0, window->width() - below.x() - 12);
                const QString shown = m_hang->fontMetrics().elidedText(text(), Qt::ElideRight, maxWidth - 20);
                if (m_hang->text() != shown) {
                    m_hang->setText(shown);
                    m_hang->adjustSize();
                }
                m_hang->move(below.x(), below.y() + 2);
                m_hang->raise();
                m_hang->show();
            }

            QPointer<QWidget> m_anchor;
            QLabel *m_hang;
            int m_want = 0;
        };

        // The button, its status line, and what the check under way has said so far.
        struct State {
            QPointer<QToolButton> button;
            QPointer<StatusLine> status;
            QTimer *spin = nullptr;
            QTimer *clear = nullptr;
            QPixmap glyph; // the refresh icon at 2x, spun while busy
            int angle = 0;
            bool manual = false;    // the user pressed «Обновить» and waits for this check
            QString ready;          // version whose verified package is downloaded
            QString progressText;   // the last line worth showing of the check under way
        };

        State &state() {
            static State st;
            return st;
        }

        void showStatus(const QString &text, bool error = false, bool fade = false) {
            auto &st = state();
            if (st.status) st.status->showText(text, error);
            if (fade) st.clear->start();
            else st.clear->stop();
        }

        // Turns the arrow inside the icon's square; the square itself stays put.
        QIcon spinFrame(const QPixmap &glyph, int angle) {
            QPixmap frame(glyph.size());
            frame.setDevicePixelRatio(glyph.devicePixelRatio());
            frame.fill(Qt::transparent);
            const QSizeF size = QSizeF(glyph.size()) / glyph.devicePixelRatio();
            const QPointF c(size.width() / 2, size.height() / 2);
            QPainter p(&frame);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setRenderHint(QPainter::Antialiasing);
            p.drawPixmap(QPointF(0, 0), glyph);
            QPainterPath circle;
            circle.addEllipse(c, size.width() * 0.36, size.height() * 0.36);
            p.setClipPath(circle);
            p.translate(c);
            p.rotate(angle);
            p.translate(-c);
            p.drawPixmap(QPointF(0, 0), glyph);
            return QIcon(frame);
        }

        void setBusy(bool busy) {
            auto &st = state();
            if (!st.button) return;
            if (busy && !st.spin->isActive()) {
                st.spin->start();
            } else if (!busy && st.spin->isActive()) {
                st.spin->stop();
                st.angle = 0;
                st.button->setIcon(QIcon(st.glyph));
            }
        }

        void setReady(const QString &version) {
            auto &st = state();
            st.ready = version;
            if (!st.button) return;
            const bool ready = !version.isEmpty();
            st.button->setProperty("slUpdateReady", ready);
            st.button->setToolTip(ready ? translate("Version %1 is ready. Press to update.").arg(version)
                                        : translate("Check for updates"));
            st.button->style()->unpolish(st.button);
            st.button->style()->polish(st.button);
        }

        // The sentence for a failed check: by the core's error kind, or by its text from
        // a core that predates kinds.
        QString errorText(const QString &kind, const QString &error) {
            QString k = kind;
            if (k.isEmpty()) {
                const QString e = error.toLower();
                if (e.contains(QStringLiteral("signature")) || e.contains(QStringLiteral("sha256")) ||
                    e.contains(QStringLiteral("mismatch")) || e.contains(QStringLiteral("refused")))
                    k = QStringLiteral("verify");
                else if (e.contains(QStringLiteral("dial")) || e.contains(QStringLiteral("timeout")) ||
                         e.contains(QStringLiteral("connection")) || e.contains(QStringLiteral("no such host")))
                    k = QStringLiteral("network");
            }
            if (k == QStringLiteral("network")) return translate("No connection to the update server");
            if (k == QStringLiteral("verify")) return translate("The update file failed verification");
            if (k == QStringLiteral("server")) return translate("The update server answered with an error");
            if (k == QStringLiteral("disk")) return translate("Could not save the update");
            return translate("Could not check for updates");
        }

        // «Готово, перезапускаю», then apply. The pause lets the line be seen.
        void installNow() {
            setBusy(true);
            showStatus(translate("Done, restarting"));
            QTimer::singleShot(600, qApp, [] {
                auto *mw = GetMainWindow();
                if (mw && mw->RequestSelfUpdate()) return;
                setBusy(false);
                state().manual = false;
                showStatus(translate("Could not start the update"), true, true);
            });
        }

        // One JSON line of `selfupdate check`: progress, or the result.
        void onProgress(const QJsonObject &line) {
            const qint64 done = line.value(QStringLiteral("done")).toInteger();
            const qint64 size = line.value(QStringLiteral("size")).toInteger();
            const int percent = size > 0 ? int(done * 100 / size) : 0;
            auto &st = state();
            st.progressText = translate("Downloading %1... %2%")
                                  .arg(line.value(QStringLiteral("version")).toString())
                                  .arg(percent);
            if (st.manual) showStatus(st.progressText);
        }

        void onResult(const QJsonObject &res, int code, const QString &stderrText) {
            auto &st = state();
            const bool manual = st.manual;
            st.manual = false;
            st.progressText.clear();
            const QString result = res.value(QStringLiteral("state")).toString();
            if (result == QStringLiteral("ready")) {
                const QString version = res.value(QStringLiteral("version")).toString();
                LOG_INFO(QStringLiteral("[update] %1 is ready").arg(version));
                setReady(version);
                if (manual) {
                    installNow();
                    return;
                }
                // Found in the background: the accent and the tooltip say so; the line stays quiet.
                setBusy(false);
                if (st.status && st.status->property("slError").toBool()) showStatus({});
            } else if (result == QStringLiteral("none")) {
                setReady({});
                setBusy(false);
                if (manual) showStatus(translate("You have the latest version %1").arg(QStringLiteral(NKR_VERSION)), false, true);
                else if (st.status && st.status->property("slError").toBool()) showStatus({});
            } else {
                // Keep whatever was ready before; a failed check is retried on the next round.
                const QString error = res.value(QStringLiteral("error")).toString();
                LOG_INFO(QStringLiteral("[update] check failed (%1): %2").arg(code).arg(error + stderrText));
                setBusy(false);
                if (manual) showStatus(errorText(res.value(QStringLiteral("kind")).toString(), error), true, true);
            }
        }

        // Starts a check, or, pressed while one runs (a background one included), waits for it.
        void runCheck(bool manual) {
            auto &st = state();
            if (manual) {
                st.manual = true;
                setBusy(true);
                showStatus(st.progressText.isEmpty() ? translate("Checking for updates...") : st.progressText);
            }
            if (currentCheck() || !st.button) return;
            if (!QFileInfo::exists(coreExe())) {
                onResult({}, -1, QStringLiteral("no ThroneCore.exe"));
                return;
            }
            auto *proc = new QProcess(st.button);
            currentCheck() = proc;
            auto buffer = std::make_shared<QByteArray>();
            auto result = std::make_shared<QJsonObject>();
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            // Each line is JSON: progress lines while it downloads, the result last.
            const auto readLines = [proc, buffer, result](bool all) {
                *buffer += proc->readAllStandardOutput();
                qsizetype nl;
                while ((nl = buffer->indexOf('\n')) >= 0 || (all && !buffer->trimmed().isEmpty())) {
                    const QByteArray line = nl >= 0 ? buffer->left(nl) : *buffer;
                    buffer->remove(0, nl >= 0 ? nl + 1 : buffer->size());
                    const auto obj = QJsonDocument::fromJson(line.trimmed()).object();
                    if (obj.value(QStringLiteral("state")).toString() == QStringLiteral("downloading")) onProgress(obj);
                    else if (!obj.isEmpty()) *result = obj;
                }
            };
            QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [readLines] { readLines(false); });
            QObject::connect(proc, &QProcess::finished, proc, [proc, readLines, result, clock](int code, QProcess::ExitStatus) {
                proc->deleteLater();
                currentCheck() = nullptr; // a press from here on starts a new check
                readLines(true);
                const QString state = result->value(QStringLiteral("state")).toString();
                // The core made the request; the journal gets its outcome, not an HTTP status.
                const bool checked = state == QStringLiteral("ready") || state == QStringLiteral("none");
                Journal::Event("api", "call", checked ? "ok" : "fail",
                               QStringLiteral("GET %1 selfupdate %2").arg(QUrl(baseUrl()).path(), checked ? state : QStringLiteral("failed")),
                               checked ? QString() : result->value(QStringLiteral("error")).toString(), clock->elapsed());
                onResult(*result, code, QString::fromUtf8(proc->readAllStandardError()).trimmed());
            });
            // A core that cannot be started never sends finished: let go of the check here.
            QObject::connect(proc, &QProcess::errorOccurred, proc, [proc](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) return;
                proc->deleteLater();
                currentCheck() = nullptr;
                onResult({}, -1, proc->errorString());
            });
            proc->start(coreExe(), {QStringLiteral("selfupdate"), QStringLiteral("check"),
                                       QStringLiteral("-base"), baseUrl(),
                                       QStringLiteral("-appid"), appId(),
                                       QStringLiteral("-code"), QString::number(SHADOWLOS_VERSION_CODE),
                                       QStringLiteral("-work"), workDir()});
        }

        // «Обновить» pressed: a downloaded update installs at once; otherwise check now.
        void onPressed() {
            auto &st = state();
            if (!st.ready.isEmpty() && !currentCheck()) {
                st.manual = false;
                installNow();
                return;
            }
            runCheck(true);
        }

        // Tells the waiting `selfupdate apply` that this build came up: the core is running.
        void confirmWhenUp(QWidget *window, const QString &dir) {
            auto *timer = new QTimer(window);
            timer->setInterval(500);
            QObject::connect(timer, &QTimer::timeout, window, [timer, dir] {
                if (!Configs::dataManager->settingsRepo->core_running) return;
                timer->stop();
                timer->deleteLater();
                QFile ok(dir + QStringLiteral("/ok"));
                if (ok.open(QIODevice::WriteOnly)) ok.close();
                LOG_INFO(QStringLiteral("[update] started as " NKR_VERSION " after an update"));
            });
            timer->start();
        }

    } // namespace

    QWidget *Install(QWidget *window, QToolButton *button) {
        auto *status = new StatusLine(button->parentWidget(), window, button);
#ifdef Q_OS_WIN
        auto &st = state();
        st.button = button;
        st.status = status;
        st.glyph = button->icon().pixmap(button->iconSize(), 2.0);
        st.spin = new QTimer(button);
        st.spin->setInterval(40);
        QObject::connect(st.spin, &QTimer::timeout, button, [] {
            auto &st = state();
            st.angle = (st.angle + 12) % 360;
            if (st.button) st.button->setIcon(spinFrame(st.glyph, st.angle));
        });
        // «Последняя версия» and errors go after a while.
        st.clear = new QTimer(button);
        st.clear->setSingleShot(true);
        st.clear->setInterval(15 * 1000);
        QObject::connect(st.clear, &QTimer::timeout, button, [] {
            if (!state().manual) showStatus({});
        });
        setReady({});
        QObject::connect(button, &QToolButton::clicked, window, [] { onPressed(); });
        if (const QString dir = pendingDir(); !dir.isEmpty()) confirmWhenUp(window, dir);

        QTimer::singleShot(kFirstCheckMs, button, [] { runCheck(false); });
        auto *every = new QTimer(button);
        every->setInterval(kCheckEveryMs);
        QObject::connect(every, &QTimer::timeout, button, [] { runCheck(false); });
        every->start();
#else
        Q_UNUSED(window)
        button->hide();
        status->hide();
#endif
        return status;
    }

    bool LaunchApply(int resumeProfileId) {
#ifdef Q_OS_WIN
        const QString work = workDir();
        QFile resume(work + QStringLiteral("/resume"));
        if (resumeProfileId >= 0 && resume.open(QIODevice::WriteOnly)) {
            resume.write(QByteArray::number(resumeProfileId));
            resume.close();
        } else {
            resume.remove();
        }
        if (auto proc = currentCheck()) {
            proc->kill();
            proc->waitForFinished(3000);
        }
        // apply runs from the app dir's own ThroneCore.exe (Windows lets a running image be
        // renamed), so nothing it runs from sits where an unelevated process could swap it.
        QStringList args{QStringLiteral("selfupdate"), QStringLiteral("apply"),
                         QStringLiteral("-dir"), QDir::toNativeSeparators(appDir()),
                         QStringLiteral("-src"), QDir::toNativeSeparators(work),
                         QStringLiteral("-exe"), QStringLiteral(SHADOWLOS_EXE_NAME ".exe"),
                         QStringLiteral("-appid"), appId(),
                         QStringLiteral("-pid"), QString::number(QCoreApplication::applicationPid())};
        LOG_INFO(QStringLiteral("[update] applying, then restarting"));
        if (dirWritable(appDir())) {
            QProcess proc;
            proc.setProgram(coreExe());
            proc.setArguments(args);
            proc.setWorkingDirectory(appDir());
            // The core is a console program; no console window should flash up.
            proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
                a->flags |= CREATE_NO_WINDOW;
            });
            if (proc.startDetached()) return true;
        } else {
            args << QStringLiteral("-deelevate");
            // (uint)-1: UAC declined or the start failed.
            if (WinCommander::runProcessElevated(coreExe(), args, appDir(), WinCommander::ShowHide, false) != uint(-1))
                return true;
        }
        LOG_INFO(QStringLiteral("[update] could not start the update; staying on " NKR_VERSION));
        return false;
#else
        Q_UNUSED(resumeProfileId)
        return false;
#endif
    }

    int TakeResumeProfile() {
#ifdef Q_OS_WIN
        const QString dir = pendingDir();
        if (dir.isEmpty()) return -1;
        QFile resume(dir + QStringLiteral("/resume"));
        if (!resume.open(QIODevice::ReadOnly)) return -1;
        bool ok = false;
        const int id = resume.readAll().trimmed().toInt(&ok);
        resume.close();
        resume.remove();
        return ok ? id : -1;
#else
        return -1;
#endif
    }

} // namespace Shadowlos::Updater
