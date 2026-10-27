#include "include/ui/shadowlos/ShadowlosUpdater.hpp"

#include "include/ui/mainwindow.h"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "include/ui/shadowlos/ShadowlosJournal.hpp"
#include "NkrVersion.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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

        void setReady(QToolButton *button, const QString &version) {
            const bool ready = !version.isEmpty();
            button->setEnabled(ready);
            button->setProperty("slUpdateReady", ready);
            button->setToolTip(ready ? translate("Version %1 is ready. Press to update.").arg(version)
                                     : translate("No updates"));
            button->style()->unpolish(button);
            button->style()->polish(button);
        }

        void runCheck(QPointer<QToolButton> button) {
            if (!QFileInfo::exists(coreExe()) || currentCheck()) return;
            auto *proc = new QProcess(button);
            currentCheck() = proc;
            QElapsedTimer clock;
            clock.start();
            QObject::connect(proc, &QProcess::finished, proc, [proc, button, clock](int code, QProcess::ExitStatus) {
                proc->deleteLater();
                const auto res = QJsonDocument::fromJson(proc->readAllStandardOutput().trimmed()).object();
                const QString state = res.value(QStringLiteral("state")).toString();
                // The core made the request; the journal gets its outcome, not an HTTP status.
                const bool checked = state == QStringLiteral("ready") || state == QStringLiteral("none");
                Journal::Event("api", "call", checked ? "ok" : "fail",
                               QStringLiteral("GET %1 selfupdate %2").arg(QUrl(baseUrl()).path(), checked ? state : QStringLiteral("failed")),
                               checked ? QString() : res.value(QStringLiteral("error")).toString(), clock.elapsed());
                if (state == QStringLiteral("ready")) {
                    const QString version = res.value(QStringLiteral("version")).toString();
                    LOG_INFO(QStringLiteral("[update] %1 is ready").arg(version));
                    if (button) setReady(button, version);
                } else if (state == QStringLiteral("none")) {
                    if (button) setReady(button, {});
                } else {
                    // Keep whatever was ready before; a failed check is retried on the next round.
                    LOG_INFO(QStringLiteral("[update] check failed (%1): %2")
                                 .arg(code)
                                 .arg(res.value(QStringLiteral("error")).toString() +
                                      QString::fromUtf8(proc->readAllStandardError()).trimmed()));
                }
            });
            proc->start(coreExe(), {QStringLiteral("selfupdate"), QStringLiteral("check"),
                                       QStringLiteral("-base"), baseUrl(),
                                       QStringLiteral("-appid"), appId(),
                                       QStringLiteral("-code"), QString::number(SHADOWLOS_VERSION_CODE),
                                       QStringLiteral("-work"), workDir()});
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

    void Install(QWidget *window, QToolButton *button) {
#ifdef Q_OS_WIN
        setReady(button, {});
        QObject::connect(button, &QToolButton::clicked, window, [] {
            if (auto *mw = GetMainWindow()) mw->RequestSelfUpdate();
        });
        if (const QString dir = pendingDir(); !dir.isEmpty()) confirmWhenUp(window, dir);

        QPointer<QToolButton> guarded(button);
        QTimer::singleShot(kFirstCheckMs, button, [guarded] { runCheck(guarded); });
        auto *every = new QTimer(button);
        every->setInterval(kCheckEveryMs);
        QObject::connect(every, &QTimer::timeout, button, [guarded] { runCheck(guarded); });
        every->start();
#else
        Q_UNUSED(window)
        button->hide();
#endif
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
