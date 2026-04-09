// Shadowlos: "Debug Check All VLess" -- IP change, 1MB TCP and UDP DNS through
// every VLESS profile, reported in a copyable dialog for support.
#include "include/ui/mainwindow.h"

#include "include/api/RPC.h"
#include "include/configs/generate.h"
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

static void showDebugCheckDialog(const QString &text) {
    runOnUiThread([text] {
        auto *dialog = new QDialog(GetMainWindow());
        dialog->setWindowTitle(QObject::tr("Debug Check Results"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->resize(700, 500);

        auto *layout = new QVBoxLayout(dialog);

        auto *edit = new QPlainTextEdit(dialog);
        edit->setReadOnly(true);
        edit->setPlainText(text);
        QFont mono("Monospace");
        mono.setStyleHint(QFont::Monospace);
        edit->setFont(mono);
        layout->addWidget(edit);

        auto *buttons = new QDialogButtonBox(dialog);
        auto *copyBtn = buttons->addButton(QObject::tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
        buttons->addButton(QDialogButtonBox::Close);
        QObject::connect(copyBtn, &QPushButton::clicked, dialog, [text] {
            QApplication::clipboard()->setText(text);
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);

        dialog->show();
    });
}

void MainWindow::check_all_vless_profiles() {
    QList<std::shared_ptr<Configs::Profile>> vlessProfiles;
    for (int gid : Configs::dataManager->groupsRepo->GetAllGroupIds()) {
        auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group) continue;
        for (int pid : group->Profiles()) {
            auto ent = Configs::dataManager->profilesRepo->GetProfile(pid);
            if (ent && (ent->type == "vless" || ent->type == "xrayvless")) vlessProfiles.append(ent);
        }
    }

    if (vlessProfiles.isEmpty()) {
        runOnUiThread([] {
            QMessageBox::information(GetMainWindow(), QObject::tr("Debug Check"),
                                     QObject::tr("No VLess profiles found."));
        }, true);
        return;
    }

    MW_show_log(tr("Starting debug check for %1 VLess profile(s)…").arg(vlessProfiles.size()));

    QStringList lines;
    lines << "=== Throne VLess Debug Check ===";
    lines << QString("Timestamp : %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    lines << QString("Profiles  : %1").arg(vlessProfiles.size());
    lines << "";

    const int timeoutMs = 30000;
    int idx = 0;

    for (const auto &ent : vlessProfiles) {
        ++idx;
        const QString profileLabel = QString("[%1/%2] %3 (%4)")
            .arg(idx).arg(vlessProfiles.size())
            .arg(ent->outbound->name)
            .arg(ent->type);
        MW_show_log(tr("Debug check: ") + ent->outbound->name);
        lines << profileLabel;

        auto buildObject = Configs::BuildTestConfig({ent});
        if (!buildObject->error.isEmpty()) {
            lines << QString("  ERROR: Could not build test config: %1").arg(buildObject->error);
            lines << "";
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
            lines << "  ERROR: Empty test config produced.";
            lines << "";
            continue;
        }
        for (const auto &xc : buildObject->xrayFullConfigs) req.xray_full_configs.push_back(xc.toStdString());
        req.xray_outbound_dns_strategy = buildObject->xrayDnsStrategy.toStdString();
        req.profile_name = ent->outbound->name.toStdString();
        req.timeout_ms = timeoutMs;

        bool rpcOK = false;
        auto result = API::defaultClient->DebugCheck(&rpcOK, req);

        if (!rpcOK) {
            lines << "  ERROR: RPC call failed (core not running?)";
            lines << "";
            continue;
        }
        if (!result.error.value().empty()) {
            lines << QString("  ERROR: %1").arg(QString::fromStdString(result.error.value()));
            lines << "";
            continue;
        }

        const QString realIP  = QString::fromStdString(result.real_ip.value());
        const QString proxyIP = QString::fromStdString(result.proxy_ip.value());

        if (result.ip_changed.value()) {
            lines << QString("  IP Check  : PASS  (%1  →  %2)").arg(realIP, proxyIP);
        } else if (proxyIP.isEmpty()) {
            lines << QString("  IP Check  : FAIL  (could not reach ipify via proxy; real IP: %1)").arg(realIP);
        } else {
            lines << QString("  IP Check  : WARN  IP did not change (real: %1, proxy: %2)").arg(realIP, proxyIP);
        }

        if (result.tcp_ok.value()) {
            lines << QString("  TCP (1MB) : PASS  (%1 bytes downloaded)").arg(result.tcp_bytes.value());
        } else {
            const QString tcpErr = QString::fromStdString(result.tcp_error.value());
            lines << QString("  TCP (1MB) : FAIL  %1").arg(tcpErr.isEmpty() ? "(unknown error)" : tcpErr);
        }

        if (result.udp_ok.value()) {
            lines << "  UDP/DNS   : PASS  (YouTube DNS query via 8.8.8.8:53 over UDP succeeded)";
        } else {
            const QString udpErr = QString::fromStdString(result.udp_error.value());
            lines << QString("  UDP/DNS   : FAIL  %1").arg(udpErr.isEmpty() ? "(unknown error)" : udpErr);
        }
        lines << "";
    }

    lines << "=== End of Debug Check ===";
    MW_show_log(tr("Debug check finished."));
    showDebugCheckDialog(lines.join("\n"));
}
