#pragma once

class QWidget;

// The routing switch on the main window: «Удобный» / «Все через VPN» /
// «Под ваш регион» (bottom bar). Each is a route profile; the switch only selects one, the way the
// Routing menu does, and restarts a running connection so it takes effect.
//
// «Под ваш регион» is a local profile we own (settings->shadowlos_regional_route).
// Its rules come from our GET /api/mobile/routing/regional?format=throne&platform=windows
// (Sl-Sub auth), fetched while the VPN is off so the server sees the user's own network;
// until the server sends a throne-route-profile document it is a copy of «Удобный».
namespace Shadowlos::Routing {

    // The switch widget; also starts the regional refresh. Call once.
    QWidget *MakeSwitch(QWidget *parent);

} // namespace Shadowlos::Routing
