#include "include/global/ShadowlosBootstrap.hpp"

#include "include/global/Configs.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/SettingsRepo.h"
#include "include/database/entities/Group.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>

namespace Shadowlos {
    namespace {
        // Only http(s) subscription URLs are provisioned. The bootstrap file
        // travels inside a downloaded archive, so treat it as untrusted input:
        // a "file://" or otherwise exotic value must never reach the updater.
        bool isAcceptableSubscriptionUrl(const QString& value) {
            const QUrl url(value, QUrl::StrictMode);
            if (!url.isValid() || url.host().isEmpty()) return false;
            const QString scheme = url.scheme().toLower();
            return scheme == "http" || scheme == "https";
        }

        // The managed group is tracked by id in settings rather than by name or
        // by Group::info -- info is overwritten with Subscription-UserInfo on
        // every successful update, so it cannot hold our marker.
        std::shared_ptr<Configs::Group> findManagedGroup() {
            const int managedId = Configs::dataManager->settingsRepo->shadowlos_managed_group;
            if (managedId < 0) return nullptr;
            return Configs::dataManager->groupsRepo->GetGroup(managedId);
        }

        // On a fresh profile initDB has just created an empty "Default" group.
        // Adopt it instead of leaving the user with an unused extra tab.
        std::shared_ptr<Configs::Group> findAdoptableGroup() {
            for (const int id : Configs::dataManager->groupsRepo->GetAllGroupIds()) {
                auto candidate = Configs::dataManager->groupsRepo->GetGroup(id);
                if (candidate != nullptr && candidate->url.isEmpty() && candidate->Profiles().isEmpty()) {
                    return candidate;
                }
            }
            return nullptr;
        }
    }

    void ApplyBootstrap() {
        QFile file(QDir::current().filePath(BootstrapFileName));
        if (!file.exists()) return; // plain upstream build, nothing to do
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "[Shadowlos] cannot open" << BootstrapFileName << file.errorString();
            return;
        }

        QJsonParseError parseError{};
        const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "[Shadowlos] malformed" << BootstrapFileName << parseError.errorString();
            return;
        }

        const QJsonObject root = doc.object();
        const QString subscriptionUrl = root["subscription_url"].toString().trimmed();
        if (!isAcceptableSubscriptionUrl(subscriptionUrl)) {
            qWarning() << "[Shadowlos] refusing non-http(s) subscription_url";
            return;
        }

        const QString requestedName = root["group_name"].toString().trimmed();
        const QString groupName = requestedName.isEmpty() ? QStringLiteral("Shadowlos") : requestedName;

        auto group = findManagedGroup();
        const bool firstRun = group == nullptr;
        if (firstRun) {
            group = findAdoptableGroup();
        }

        if (group == nullptr) {
            auto created = Configs::GroupsRepo::NewGroup();
            created->name = groupName;
            created->url = subscriptionUrl;
            if (!Configs::dataManager->groupsRepo->AddGroup(created)) {
                qWarning() << "[Shadowlos] failed to create subscription group";
                return;
            }
            group = created;
        } else {
            // Name is only set when first taking the group over; a later rename
            // by the user is theirs to keep.
            if (firstRun) group->name = groupName;
            group->url = subscriptionUrl;
            // Auto-update must stay reachable: a group left archived or skipped
            // would silently stop receiving new servers.
            group->skip_auto_update = false;
            group->archive = false;
            Configs::dataManager->groupsRepo->Save(group);
        }

        bool settingsDirty = false;
        if (Configs::dataManager->settingsRepo->shadowlos_managed_group != group->id) {
            Configs::dataManager->settingsRepo->shadowlos_managed_group = group->id;
            settingsDirty = true;
        }

        // Make the recurring refresh actually run: SettingsRepo treats a
        // negative value as "disabled", and the shipped default is -30.
        const int autoUpdateMinutes = root["auto_update_minutes"].toInt(60);
        if (autoUpdateMinutes >= 30 && Configs::dataManager->settingsRepo->sub_auto_update < autoUpdateMinutes) {
            Configs::dataManager->settingsRepo->sub_auto_update = autoUpdateMinutes;
            settingsDirty = true;
        }

        // Point the UI at the provisioned group when the remembered one is gone.
        if (Configs::dataManager->groupsRepo->GetGroup(Configs::dataManager->settingsRepo->current_group) == nullptr) {
            Configs::dataManager->settingsRepo->current_group = group->id;
            settingsDirty = true;
        }

        if (settingsDirty) Configs::dataManager->settingsRepo->Save();
    }
}
