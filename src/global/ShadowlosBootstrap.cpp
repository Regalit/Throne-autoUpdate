#include "include/global/ShadowlosBootstrap.hpp"

#include "include/global/Configs.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/database/SettingsRepo.h"
#include "include/database/entities/Group.h"
#include "include/database/entities/RouteProfile.h"
#include "include/database/entities/RouteRule.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
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

        // Set only when the bootstrap file asks for it; read later by MainWindow.
        bool tunOnStart = false;

        QList<QString> stringList(const QJsonValue& value) {
            QList<QString> out;
            for (const auto& item : value.toArray()) {
                const QString s = item.toString().trimmed();
                if (!s.isEmpty()) out << s;
            }
            return out;
        }

        // Build one rule from the bootstrap's routing schema.
        //
        // Deliberately not routed through RouteProfile::FromShareInput: that path
        // reads the outbound from a key named "outbound" and expects the rule type
        // as a string token. Our preset carries neither, so every rule would
        // silently fall back to proxy -- i.e. .ru traffic would be tunnelled, the
        // exact opposite of the intent.
        std::shared_ptr<Configs::RouteRule> ruleFromJson(const QJsonObject& obj) {
            auto rule = std::make_shared<Configs::RouteRule>();
            rule->name = obj.value("name").toString();

            const QString action = obj.value("action").toString();
            if (!action.isEmpty()) rule->action = action;
            const QString protocol = obj.value("protocol").toString();
            if (!protocol.isEmpty()) rule->protocol = protocol;

            rule->domain_suffix = stringList(obj.value("domain_suffix"));
            rule->domain = stringList(obj.value("domain"));
            rule->domain_keyword = stringList(obj.value("domain_keyword"));
            rule->process_name = stringList(obj.value("process_name"));
            rule->rule_set = stringList(obj.value("rule_set"));

            // "direct" | "proxy" | "block"; anything else falls back to proxy,
            // which is the safe default for an unrecognised value.
            rule->outboundID = Configs::stringToOutboundID(obj.value("outbound").toString());

            // Type drives how the UI renders the rule. The enum is persisted as a
            // raw int, so map from stable names rather than trusting numbers from
            // the file.
            const QString type = obj.value("type").toString();
            if (type == "simple_address_proxy") rule->type = Configs::simpleAddressProxy;
            else if (type == "simple_address_bypass") rule->type = Configs::simpleAddressBypass;
            else if (type == "simple_process_name_proxy") rule->type = Configs::simpleProcessNameProxy;
            else if (type == "simple_process_name_bypass") rule->type = Configs::simpleProcessNameBypass;
            else rule->type = Configs::custom;

            return rule;
        }

        // Install the routing preset, replacing the rules of the profile we own.
        // Unlike the subscription URL this is revision-gated: it lands once and is
        // only re-applied when the archiver bumps "revision", so a user's own
        // routing edits survive an ordinary restart.
        void applyRouting(const QJsonObject& routing) {
            const QJsonArray rules = routing.value("rules").toArray();
            if (rules.isEmpty()) return;

            const int revision = routing.value("revision").toInt(1);
            auto* settings = Configs::dataManager->settingsRepo;
            const int managedId = settings->shadowlos_managed_route;
            auto existing = managedId < 0 ? nullptr
                                          : Configs::dataManager->routesRepo->GetRouteProfile(managedId);
            if (existing != nullptr && settings->shadowlos_routing_revision >= revision) {
                // Already installed and unchanged; leave the user's edits alone.
                settings->current_route_id = existing->id;
                return;
            }

            auto profile = existing != nullptr ? existing : Configs::RoutesRepo::NewRouteProfile();
            profile->name = routing.value("name").toString().trimmed().isEmpty()
                                ? QStringLiteral("Shadowlos")
                                : routing.value("name").toString().trimmed();
            profile->defaultOutboundID = Configs::stringToOutboundID(
                routing.value("default_outbound").toString());
            profile->Rules.clear();
            for (const auto& value : rules) {
                if (!value.isObject()) continue;
                profile->Rules << ruleFromJson(value.toObject());
            }

            if (existing != nullptr) {
                Configs::dataManager->routesRepo->Save(profile);
            } else if (!Configs::dataManager->routesRepo->AddRouteProfile(profile)) {
                qWarning() << "[Shadowlos] failed to create routing profile";
                return;
            }

            settings->shadowlos_managed_route = profile->id;
            settings->shadowlos_routing_revision = revision;
            settings->current_route_id = profile->id;
            settings->active_routing = profile->name;
        }
    }

    bool WantsTunOnStart() {
        return tunOnStart;
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

        // Routing preset (.ru and friends direct, everything else tunnelled).
        if (root.contains("routing")) {
            applyRouting(root.value("routing").toObject());
            settingsDirty = true;
        }

        // TUN mode. Only the intent is recorded here: actually enabling it can
        // raise a privilege prompt, so MainWindow flips it once the UI is up.
        if (root.value("enable_tun").toBool()) {
            tunOnStart = true;
            if (!Configs::dataManager->settingsRepo->enable_tun_routing) {
                Configs::dataManager->settingsRepo->enable_tun_routing = true;
                settingsDirty = true;
            }
        }

        if (settingsDirty) Configs::dataManager->settingsRepo->Save();
    }
}
