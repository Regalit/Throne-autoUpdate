#include "include/ui/shadowlos/ShadowlosUpdater.hpp"

#include "include/ui/mainwindow.h"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "NkrVersion.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
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

#ifdef Q_OS_WIN
#include "3rdparty/WinCommander.hpp"
#include <windows.h>
#endif

namespace Shadowlos::Updater {

    namespace {

        constexpr int kFirstCheckMs = 30 * 1000;
        constexpr int kCheckEveryMs = 6 * 60 * 60 * 1000;

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

        // update\ next to the exe; %LOCALAPPDATA%\<exe name>\update when the install folder
        // is read-only (Program Files), where applying then asks for UAC once.
        QString workDir() {
            static const QString dir = [] {
                const QString local = appDir() + QStringLiteral("/update");
                if (QDir().mkpath(local) && dirWritable(local)) return local;
                const QString user = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                                     QStringLiteral("/" SHADOWLOS_EXE_NAME "/update");
                QDir().mkpath(user);
                return user;
            }();
            return dir;
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

        bool afterUpdate() {
            return QCoreApplication::arguments().contains(QStringLiteral("--after-update"));
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
            if (!QFileInfo::exists(coreExe())) return;
            auto *proc = new QProcess(button);
            QObject::connect(proc, &QProcess::finished, proc, [proc, button](int code, QProcess::ExitStatus) {
                proc->deleteLater();
                const auto res = QJsonDocument::fromJson(proc->readAllStandardOutput().trimmed()).object();
                const QString state = res.value(QStringLiteral("state")).toString();
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
        void confirmWhenUp(QWidget *window) {
            auto *timer = new QTimer(window);
            timer->setInterval(500);
            QObject::connect(timer, &QTimer::timeout, window, [timer] {
                if (!Configs::dataManager->settingsRepo->core_running) return;
                timer->stop();
                timer->deleteLater();
                QFile ok(workDir() + QStringLiteral("/ok"));
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
        if (afterUpdate()) confirmWhenUp(window);

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
        // Run a copy of the core, so ThroneCore.exe itself can be replaced.
        const QString runner = work + QStringLiteral("/selfupdate-run.exe");
        QFile::remove(runner);
        if (!QFile::copy(coreExe(), runner)) {
            LOG_INFO(QStringLiteral("[update] cannot copy ThroneCore.exe"));
            return false;
        }
        const QStringList args{QStringLiteral("selfupdate"), QStringLiteral("apply"),
                               QStringLiteral("-dir"), QDir::toNativeSeparators(appDir()),
                               QStringLiteral("-work"), QDir::toNativeSeparators(work),
                               QStringLiteral("-exe"), QStringLiteral(SHADOWLOS_EXE_NAME ".exe"),
                               QStringLiteral("-pid"), QString::number(QCoreApplication::applicationPid())};
        LOG_INFO(QStringLiteral("[update] applying, then restarting"));
        if (dirWritable(appDir())) {
            QProcess proc;
            proc.setProgram(runner);
            proc.setArguments(args);
            proc.setWorkingDirectory(appDir());
            // The core is a console program; no console window should flash up.
            proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
                a->flags |= CREATE_NO_WINDOW;
            });
            return proc.startDetached();
        }
        WinCommander::runProcessElevated(runner, args, appDir(), WinCommander::ShowHide, false);
        return true;
#else
        Q_UNUSED(resumeProfileId)
        return false;
#endif
    }

    int TakeResumeProfile() {
#ifdef Q_OS_WIN
        if (!afterUpdate()) return -1;
        QFile resume(workDir() + QStringLiteral("/resume"));
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
