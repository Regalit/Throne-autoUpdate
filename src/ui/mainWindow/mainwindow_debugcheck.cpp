// Shadowlos: "Debug Check All VLess" -- IP change, 1MB TCP and UDP DNS through
// every VLESS profile, reported in a copyable dialog for support.
#include "include/ui/mainwindow.h"

#include "include/api/RPC.h"
#include "include/configs/generate.h"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollBar>
#include <QSemaphore>
#include <QThread>

#include <memory>

// Each profile occupies 5 lines: header, IP, TCP, UDP, blank.
static const int LINES_PER_PROFILE = 5;
// Lines before the first profile block: title, timestamp, count, sub status, blank.
static const int HEADER_LINES = 5;

static QString pendingBlock(int idx, int total, const QString &name, const QString &type) {
    return QString("[%1/%2] %3 (%4)\n"
                   "  IP Check  : PENDING\n"
                   "  TCP (1MB) : PENDING\n"
                   "  UDP/DNS   : PENDING\n")
        .arg(idx).arg(total).arg(name).arg(type);
}

// Refreshes every subscription group and waits for them, at most timeoutMs.
static void refreshSubscriptionsAndWait(int timeoutMs) {
    QList<int> gids;
    for (const int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (group && !group->url.isEmpty() && !group->archive) gids << gid;
    }
    // Shared: a refresh that outlives the timeout still releases into a live semaphore.
    auto done = std::make_shared<QSemaphore>();
    for (const int gid : gids) Subscription::updater()->RefreshGroup(gid, [done] { done->release(); });
    done->tryAcquire(gids.size(), timeoutMs);
}

void MainWindow::check_all_vless_profiles() {
    auto collectVless = [] {
        QList<std::shared_ptr<Configs::Profile>> result;
        for (int gid : Configs::dataManager->groupsRepo->GetAllGroupIds()) {
            auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            for (int pid : group->Profiles()) {
                auto ent = Configs::dataManager->profilesRepo->GetProfile(pid);
                if (ent && (ent->type == "vless" || ent->type == "xrayvless")) result.append(ent);
            }
        }
        return result;
    };

    auto initialProfiles = collectVless();
    if (initialProfiles.isEmpty()) {
        runOnUiThread([] {
            QMessageBox::information(GetMainWindow(), QObject::tr("Debug Check"),
                                     QObject::tr("No VLess profiles found."));
        }, true);
        return;
    }

    QString initialText;
    initialText += "=== Throne VLess Debug Check ===\n";
    initialText += QString("Timestamp : %1\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    initialText += QString("Profiles  : %1\n").arg(initialProfiles.size());
    initialText += "Subscriptions : Updating...\n";
    initialText += "\n";
    for (int i = 0; i < initialProfiles.size(); ++i)
        initialText += pendingBlock(i + 1, initialProfiles.size(),
                                    initialProfiles[i]->outbound->name, initialProfiles[i]->type);
    initialText += "=== Running... ===";

    QPointer<QPlainTextEdit> edit;
    QPointer<QPushButton> copyBtn;
    runOnUiThread([&] {
        auto *dialog = new QDialog(GetMainWindow());
        dialog->setWindowTitle(QObject::tr("Debug Check Results"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->resize(700, 500);

        auto *layout = new QVBoxLayout(dialog);

        edit = new QPlainTextEdit(dialog);
        edit->setReadOnly(true);
        edit->setPlainText(initialText);
        QFont mono("Monospace");
        mono.setStyleHint(QFont::Monospace);
        edit->setFont(mono);
        layout->addWidget(edit);

        auto *supportLabel = new QLabel(
            QObject::tr("After this is complete, copy the output using the button and send it to support.\n"
                        "Debug info does not contain any personal information."),
            dialog);
        supportLabel->setWordWrap(true);
        layout->addWidget(supportLabel);

        auto *buttons = new QDialogButtonBox(dialog);
        copyBtn = buttons->addButton(QObject::tr("Wait..."), QDialogButtonBox::ActionRole);
        copyBtn->setEnabled(false);
        QPointer<QPlainTextEdit> editRef = edit;
        QObject::connect(copyBtn, &QPushButton::clicked, dialog, [editRef] {
            if (editRef) QApplication::clipboard()->setText(editRef->toPlainText());
        });
        layout->addWidget(buttons);

        dialog->show();
    }, true); // wait so the pointers are set before we continue

    auto updateEdit = [edit](const QString &text) {
        runOnUiThread([edit, text] {
            if (!edit) return;
            int scrollPos = edit->verticalScrollBar()->value();
            edit->setPlainText(text);
            edit->verticalScrollBar()->setValue(scrollPos);
        });
    };

    MW_show_log(tr("Debug check: updating subscriptions..."));
    refreshSubscriptionsAndWait(60000);

    // The refresh may have added or removed profiles.
    auto profiles = collectVless();
    const int total = profiles.size();
    MW_show_log(tr("Debug check: starting checks for %1 VLess profile(s)...").arg(total));

    QStringList lines;
    lines << "=== Throne VLess Debug Check ===";
    lines << QString("Timestamp : %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    lines << QString("Profiles  : %1").arg(total);
    lines << "Subscriptions : Updated";
    lines << "";
    for (int i = 0; i < total; ++i) {
        lines << QString("[%1/%2] %3 (%4)").arg(i + 1).arg(total)
                     .arg(profiles[i]->outbound->name).arg(profiles[i]->type);
        lines << "  IP Check  : PENDING";
        lines << "  TCP (1MB) : PENDING";
        lines << "  UDP/DNS   : PENDING";
        lines << "";
    }
    lines << "=== Running... ===";
    updateEdit(lines.join("\n"));

    auto setProfileLines = [&](int idx, const QString &ip, const QString &tcp, const QString &udp) {
        int base = HEADER_LINES + idx * LINES_PER_PROFILE;
        lines[base + 1] = ip;
        lines[base + 2] = tcp;
        lines[base + 3] = udp;
        updateEdit(lines.join("\n"));
    };

    const int timeoutMs = 30000;

    for (int idx = 0; idx < total; ++idx) {
        const auto &ent = profiles[idx];
        const int base = HEADER_LINES + idx * LINES_PER_PROFILE;

        lines[base + 1] = "  IP Check  : RUNNING";
        lines[base + 2] = "  TCP (1MB) : RUNNING";
        lines[base + 3] = "  UDP/DNS   : RUNNING";
        updateEdit(lines.join("\n"));

        MW_show_log(tr("Debug check [%1/%2]: %3").arg(idx + 1).arg(total).arg(ent->outbound->name));

        auto buildObject = Configs::BuildTestConfig({ent});
        if (!buildObject->error.isEmpty()) {
            setProfileLines(idx, QString("  ERROR: Could not build test config: %1").arg(buildObject->error), "", "");
            continue;
        }

        libcore::DebugCheckRequest req;
        if (buildObject->fullConfigs.contains(ent->id)) {
            req.config = buildObject->fullConfigs[ent->id].toStdString();
            req.use_default_outbound = true;
        } else if (!buildObject->outboundTags.isEmpty()) {
            req.config = QJsonObject2QString(buildObject->coreConfig, false).toStdString();
            req.outbound_tag = buildObject->outboundTags.first().toStdString();
            req.use_default_outbound = false;
            req.need_xray = buildObject->isXrayNeeded;
            if (buildObject->isXrayNeeded) req.xray_config = QJsonObject2QString(buildObject->xrayConfig, false).toStdString();
        } else {
            setProfileLines(idx, "  ERROR: Empty test config produced.", "", "");
            continue;
        }
        for (const auto &xc : buildObject->xrayFullConfigs) req.xray_full_configs.push_back(xc.toStdString());
        req.xray_outbound_dns_strategy = buildObject->xrayDnsStrategy.toStdString();
        req.profile_name = ent->outbound->name.toStdString();
        req.timeout_ms = timeoutMs;

        bool rpcOK = false;
        auto result = API::defaultClient->DebugCheck(&rpcOK, req);

        if (!rpcOK) {
            setProfileLines(idx, "  ERROR: RPC call failed (core not running?)", "", "");
            continue;
        }
        if (!result.error.value().empty()) {
            setProfileLines(idx, QString("  ERROR: %1").arg(QString::fromStdString(result.error.value())), "", "");
            continue;
        }

        const QString proxyIP = QString::fromStdString(result.proxy_ip.value());
        QString ipLine, tcpLine, udpLine;

        if (result.ip_changed.value())
            ipLine = QString("  IP Check  : PASS  ([hidden]  →  %1)").arg(proxyIP);
        else if (proxyIP.isEmpty())
            ipLine = QString("  IP Check  : FAIL  (could not reach ipify)");
        else
            ipLine = QString("  IP Check  : WARN  IP unchanged (proxy: %1)").arg(proxyIP);

        if (result.tcp_ok.value())
            tcpLine = QString("  TCP (1MB) : PASS  (%1 bytes)").arg(result.tcp_bytes.value());
        else {
            const QString e = QString::fromStdString(result.tcp_error.value());
            tcpLine = QString("  TCP (1MB) : FAIL  %1").arg(e.isEmpty() ? "(unknown)" : e);
        }

        if (result.udp_ok.value())
            udpLine = "  UDP/DNS   : PASS  (youtube.com DNS via 8.8.8.8:53 over UDP)";
        else {
            const QString e = QString::fromStdString(result.udp_error.value());
            udpLine = QString("  UDP/DNS   : FAIL  %1").arg(e.isEmpty() ? "(unknown)" : e);
        }

        setProfileLines(idx, ipLine, tcpLine, udpLine);
    }

    lines.last() = "=== Done ===";
    updateEdit(lines.join("\n"));
    runOnUiThread([copyBtn] {
        if (!copyBtn) return;
        copyBtn->setText(QObject::tr("Copy to Clipboard"));
        copyBtn->setEnabled(true);
    });
    MW_show_log(tr("Debug check finished."));
}
