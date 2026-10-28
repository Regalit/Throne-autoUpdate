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
#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include <climits>
#include <thread>

namespace Shadowlos::Routing {

    namespace {

        // Until 1.0.13 «Настройка под ваш регион»; MakeSwitch renames a profile made before.
        const QString RegionalName = QStringLiteral("Под ваш регион");
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

        // «Все через VPN», found by name once; looked up again only when that id is gone.
        std::shared_ptr<Configs::RouteProfile> allVpn() {
            static int cachedId = -1;
            if (auto p = route(cachedId); p && p->name == Configs::RouteProfile::DefaultChainName()) return p;
            cachedId = -1;
            for (const auto &p : Configs::dataManager->routesRepo->GetAllRouteProfiles()) {
                if (p != nullptr && p->name == Configs::RouteProfile::DefaultChainName()) {
                    cachedId = p->id;
                    return p;
                }
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
            Journal::Event("app", "settings", "ok", QStringLiteral("route ") + profile->name);
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
            QString token;
            // The Sl-Sub token comes from the managed subscription; nothing without it.
            if (!ApiToken(&token)) return;
            nextRefresh() = now + kRetryAfterSecs;
            const QString url = QLatin1String(ApiBase) + QStringLiteral("/api/mobile/routing/regional?format=throne&platform=windows");
            const QString id = InstallId();
            // QPointer: the answer is dropped if the app is going away meanwhile.
            QPointer<QCoreApplication> app(qApp);
            std::thread([url, token, id, app] {
                Configs_network::HttpGetOptions options;
                options.maxBytes = 1 << 20;
                options.direct = true;
                options.headers = {{"Authorization", "Sl-Sub " + token.toUtf8()},
                                   {"X-Sl-Install-Id", id.toUtf8()},
                                   {"X-Sl-Platform", "windows"}};
                QElapsedTimer clock;
                clock.start();
                const auto resp = Configs_network::NetworkRequestHelper::HttpGet(url, options);
                Journal::Event("api", "call", resp.error.isEmpty() ? "ok" : "fail",
                               QStringLiteral("GET /api/mobile/routing/regional ") + (resp.error.isEmpty() ? "200" : "error"),
                               resp.error, clock.elapsed());
                if (app.isNull() || QCoreApplication::closingDown()) return;
                QMetaObject::invokeMethod(app.data(), [resp] {
                    if (settings()->started_id >= 0) return; // connected meanwhile: next time, off the VPN
                    if (!resp.error.isEmpty()) {
                        LOG_INFO(QStringLiteral("[routing] regional request failed: %1").arg(resp.error));
                        // The fallback copy follows «Удобный» while the server cannot be reached.
                        if (const auto profile = route(settings()->shadowlos_regional_route)) {
                            copyComfyInto(profile);
                            Configs::dataManager->routesRepo->Save(profile);
                        }
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
        // The regional profile's old name, «Настройка под ваш регион», shortened.
        if (const auto profile = route(settings()->shadowlos_regional_route); profile && profile->name != RegionalName) {
            if (settings()->active_routing == profile->name) settings()->active_routing = RegionalName;
            profile->name = RegionalName;
            Configs::dataManager->routesRepo->Save(profile);
            settings()->Save();
        }
        auto *box = new QWidget(parent);
        box->setObjectName(QStringLiteral("slRouteSwitch"));
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto *group = new QButtonGroup(box);
        group->setExclusive(false); // none checked while the user's own profile is active

        const struct { int mode; QString text; QString tip; } items[] = {
            {Comfy, ComfyRouteProfileName(), translate("Russian sites directly, everything else through the VPN")},
            {AllVpn, Configs::RouteProfile::DefaultChainName(), translate("All traffic through the VPN")},
            {Regional, RegionalName, translate("Rules picked for your region and provider")},
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
        // The Routing menu, the tray and the routes dialog change the profile too; only a
        // change of the selected id is worth reloading profiles for, plus a slow resync.
        auto *poll = new QTimer(box);
        poll->setInterval(1000);
        QObject::connect(poll, &QTimer::timeout, box, [sync] {
            static int lastRoute = INT_MIN;
            static int ticks = 0;
            if (settings()->current_route_id != lastRoute || ++ticks % 30 == 0) {
                lastRoute = settings()->current_route_id;
                sync();
            }
            maybeRefresh();
        });
        poll->start();
        sync();
        QTimer::singleShot(3000, box, [] { maybeRefresh(); });
        return box;
    }

} // namespace Shadowlos::Routing
