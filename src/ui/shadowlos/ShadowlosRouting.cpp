#include "include/ui/shadowlos/ShadowlosRouting.hpp"

#include "include/ui/mainwindow.h"

#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/database/SettingsRepo.h"
#include "include/database/entities/RouteProfile.h"
#include "include/global/HTTPRequestHelper.hpp"
#include "include/global/Logger.hpp"
#include "include/global/ShadowlosBootstrap.hpp"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <thread>

namespace Shadowlos::Routing {

    namespace {

        const QString RegionalName = QStringLiteral("Настройка под ваш регион");
        constexpr qint64 kRefreshEverySecs = 24 * 60 * 60;
        constexpr qint64 kRetryAfterSecs = 30 * 60;

        enum Mode { Comfy = 0, Regional = 1, AllVpn = 2 };

        QString translate(const char *text) {
            return QCoreApplication::translate("ShadowlosRouting", text);
        }

        Configs::SettingsRepo *settings() {
            return Configs::dataManager->settingsRepo.get();
        }

        std::shared_ptr<Configs::RouteProfile> route(int id) {
            return id >= 0 ? Configs::dataManager->routesRepo->GetRouteProfile(id) : nullptr;
        }

        std::shared_ptr<Configs::RouteProfile> comfy() {
            return route(settings()->shadowlos_managed_route);
        }

        std::shared_ptr<Configs::RouteProfile> allVpn() {
            for (const auto &p : Configs::dataManager->routesRepo->GetAllRouteProfiles()) {
                if (p != nullptr && p->name == Configs::RouteProfile::DefaultChainName()) return p;
            }
            return nullptr;
        }

        // Gives the regional profile «Удобный»'s rules: right while the server's regional
        // answer is the global «Удобный», and the fallback before its first answer.
        void copyComfyInto(const std::shared_ptr<Configs::RouteProfile> &target) {
            const auto source = comfy();
            if (source == nullptr || target == nullptr) return;
            target->defaultOutboundID = source->defaultOutboundID;
            target->Rules.clear();
            for (const auto &rule : source->Rules) target->Rules << std::make_shared<Configs::RouteRule>(*rule);
        }

        // The regional profile, made on first use as a copy of «Удобный».
        std::shared_ptr<Configs::RouteProfile> regional() {
            if (auto existing = route(settings()->shadowlos_regional_route)) return existing;
            if (comfy() == nullptr) return nullptr;
            auto profile = Configs::RoutesRepo::NewRouteProfile();
            profile->name = RegionalName;
            copyComfyInto(profile);
            if (!Configs::dataManager->routesRepo->AddRouteProfile(profile)) return nullptr;
            settings()->shadowlos_regional_route = profile->id;
            settings()->Save();
            return profile;
        }

        std::shared_ptr<Configs::RouteProfile> profileFor(int mode) {
            switch (mode) {
                case Comfy: return comfy();
                case Regional: return regional();
                default: return allVpn();
            }
        }

        int currentMode() {
            const int cur = settings()->current_route_id;
            if (const auto p = comfy(); p && p->id == cur) return Comfy;
            if (const auto p = route(settings()->shadowlos_regional_route); p && p->id == cur) return Regional;
            if (const auto p = allVpn(); p && p->id == cur) return AllVpn;
            return -1; // the user's own profile, picked in the Routing menu
        }

        // As the Routing menu does: select, save, and restart a running connection.
        void select(int mode) {
            const auto profile = profileFor(mode);
            if (profile == nullptr || settings()->current_route_id == profile->id) return;
            settings()->current_route_id = profile->id;
            settings()->active_routing = profile->name;
            settings()->Save();
            LOG_INFO(QStringLiteral("[routing] switched to %1").arg(profile->name));
            if (settings()->started_id >= 0) {
                if (auto *mw = GetMainWindow()) mw->profile_start(settings()->started_id);
            }
        }

        QString installId() {
            if (QUuid(settings()->shadowlos_install_id).isNull()) {
                settings()->shadowlos_install_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                settings()->Save();
            }
            return settings()->shadowlos_install_id;
        }

        // The base URL and Sl-Sub token come from the managed subscription
        // (…/dynamic_config?user_id_hash=<64 hex>…); nothing without it.
        bool apiFromSubscription(QString *base, QString *token) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(settings()->shadowlos_managed_group);
            if (group == nullptr) return false;
            const QUrl url(group->url);
            *token = QUrlQuery(url).queryItemValue(QStringLiteral("user_id_hash"));
            if (!url.isValid() || url.host().isEmpty() || token->size() != 64) return false;
            *base = url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
            return true;
        }

        // Applies a regional answer on the UI thread. A throne-route-profile document
        // replaces the rules; anything else (the apps' format, before the server sends
        // ours) leaves the profile a copy of «Удобный».
        void applyRegional(const QJsonObject &answer) {
            const auto profile = regional();
            if (profile == nullptr) return;
            const QJsonObject doc = answer.value(QStringLiteral("doc")).toObject();
            const QJsonArray rules = doc.value(QStringLiteral("rules")).toArray();
            if (doc.value(QStringLiteral("kind")).toString() == QStringLiteral("throne-route-profile") && !rules.isEmpty()) {
                profile->defaultOutboundID = Configs::stringToOutboundID(doc.value(QStringLiteral("default_outbound")).toString());
                profile->Rules.clear();
                for (const auto &value : rules) {
                    if (value.isObject()) profile->Rules << Shadowlos::RuleFromRoutingJson(value.toObject());
                }
                LOG_INFO(QStringLiteral("[routing] regional rules for cell \"%1\" (%2 rules)")
                             .arg(answer.value(QStringLiteral("cell")).toString())
                             .arg(profile->Rules.size()));
            } else {
                copyComfyInto(profile);
                LOG_INFO(QStringLiteral("[routing] regional answer has no Throne rules; using «Удобный»"));
            }
            profile->name = RegionalName;
            Configs::dataManager->routesRepo->Save(profile);
        }

        qint64 &nextRefresh() {
            static qint64 at = 0;
            return at;
        }

        // Fetches the regional rules while the VPN is off: through the tunnel the
        // server would see our exit, not the user's region.
        void maybeRefresh() {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (now < nextRefresh() || settings()->started_id >= 0) return;
            QString base, token;
            if (!apiFromSubscription(&base, &token)) return;
            nextRefresh() = now + kRetryAfterSecs;
            const QString url = base + QStringLiteral("/api/mobile/routing/regional?format=throne&platform=windows");
            const QString id = installId();
            std::thread([url, token, id] {
                Configs_network::HttpGetOptions options;
                options.maxBytes = 1 << 20;
                options.headers = {{"Authorization", "Sl-Sub " + token.toUtf8()},
                                   {"X-Sl-Install-Id", id.toUtf8()},
                                   {"X-Sl-Platform", "windows"}};
                const auto resp = Configs_network::NetworkRequestHelper::HttpGet(url, options);
                QMetaObject::invokeMethod(qApp, [resp] {
                    if (!resp.error.isEmpty()) {
                        LOG_INFO(QStringLiteral("[routing] regional request failed: %1").arg(resp.error));
                        return;
                    }
                    const auto answer = QJsonDocument::fromJson(resp.data).object();
                    if (answer.isEmpty()) {
                        LOG_INFO(QStringLiteral("[routing] regional answer is not JSON"));
                        return;
                    }
                    applyRegional(answer);
                    nextRefresh() = QDateTime::currentSecsSinceEpoch() + kRefreshEverySecs;
                });
            }).detach();
        }

    } // namespace

    QWidget *MakeSwitch(QWidget *parent) {
        auto *box = new QWidget(parent);
        box->setObjectName(QStringLiteral("slRouteSwitch"));
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto *group = new QButtonGroup(box);
        group->setExclusive(false); // none checked while the user's own profile is active

        const struct { int mode; QString text; QString tip; } items[] = {
            {Comfy, ComfyRouteProfileName(), translate("Russian sites directly, everything else through the VPN")},
            {Regional, RegionalName, translate("Rules picked for your region and provider")},
            {AllVpn, Configs::RouteProfile::DefaultChainName(), translate("All traffic through the VPN")},
        };
        for (const auto &item : items) {
            auto *button = new QToolButton(box);
            button->setObjectName(QStringLiteral("slRouteMode%1").arg(item.mode));
            button->setText(item.text);
            button->setToolTip(item.tip);
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setProperty("slRole", "segment");
            group->addButton(button, item.mode);
            row->addWidget(button);
        }

        const auto sync = [group] {
            const int mode = currentMode();
            for (auto *button : group->buttons()) {
                const int id = group->id(button);
                button->setChecked(id == mode);
                // «Удобный» comes with the archive, «Все через VPN» can be deleted in the menu.
                button->setEnabled(id == Regional ? comfy() != nullptr : profileFor(id) != nullptr);
            }
        };
        QObject::connect(group, &QButtonGroup::idClicked, box, [sync](int mode) {
            select(mode);
            sync();
        });
        // The Routing menu, the tray and the routes dialog change the profile too.
        auto *poll = new QTimer(box);
        poll->setInterval(1000);
        QObject::connect(poll, &QTimer::timeout, box, [sync] {
            sync();
            maybeRefresh();
        });
        poll->start();
        sync();
        QTimer::singleShot(3000, box, [] { maybeRefresh(); });
        return box;
    }

} // namespace Shadowlos::Routing
