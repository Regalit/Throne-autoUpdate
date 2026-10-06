#pragma once

#include <QString>

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

    // True when the bootstrap file asked for TUN mode to start enabled.
    // Consulted by MainWindow once the UI exists -- enabling TUN can trigger a
    // privilege-elevation prompt, so it must not happen during initDB.
    bool WantsTunOnStart();
}
