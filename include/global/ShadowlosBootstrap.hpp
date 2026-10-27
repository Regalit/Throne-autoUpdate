#pragma once

#include <QJsonObject>
#include <QString>

#include <memory>

namespace Configs { class RouteRule; }

// Shadowlos provisioning.
//
// Shadowlos ships this client as a per-user archive. Rather than patching the
// binary throne.db before download, the archive carries a plaintext
// "shadowlos.json" next to it that names the user's subscription URL and the
// routing preset to install. This is applied on every launch, so a user who
// breaks their subscription group or routing gets it healed on restart.
//
// Historically the archive also shipped a Throne_*.json settings file, but
// Throne moved its settings and routing into SQLite, so that path is dead.
namespace Shadowlos {
    // Filename expected in the config directory (alongside throne.db).
    inline constexpr auto BootstrapFileName = "shadowlos.json";

    // Reads the bootstrap file from the current directory and reconciles the
    // managed subscription group: created if missing, URL re-asserted if the
    // user changed it. Also installs the routing preset and any settings the
    // file pins. Without a readable file it only does the one-time switch to the
    // Shadowlos theme (see ShadowlosChrome.hpp).
    //
    // Must be called after Configs::initDB (needs dataManager).
    void ApplyBootstrap();

    // One rule of a throne-route-profile document (shadowlos.json "routing", the
    // regional routing endpoint), with the outbound and type tokens our server uses.
    std::shared_ptr<Configs::RouteRule> RuleFromRoutingJson(const QJsonObject& obj);

    // A fresh install whose managed group is still empty gets the servers that came in
    // the archive ("static_profiles"), so it works even when the first subscription
    // refresh fails. Call once the subscription updater exists, before its first refresh.
    void ImportStaticProfilesIfEmpty();

    // The name of the managed profile provisioned from shadowlos.json.
    QString ComfyRouteProfileName();

    // Our mobile API. Always this host: the subscription's own host (shadowlos.xyz)
    // answers 404 to /api/mobile/*.
    inline constexpr auto ApiBase = "https://master.shadowlos.xyz";

    // The Sl-Sub token for ApiBase: the user_id_hash (64 hex) of the managed
    // subscription's URL (…/dynamic_config?user_id_hash=…); false without it. The
    // token is a secret: never log it.
    bool ApiToken(QString *token);

    // settings->shadowlos_install_id (UUID v4), made on first use; X-Sl-Install-Id.
    QString InstallId();

    // True when the bootstrap file asked for TUN mode to start enabled.
    // Consulted by MainWindow once the UI exists -- enabling TUN can trigger a
    // privilege-elevation prompt, so it must not happen during initDB.
    bool WantsTunOnStart();
}
