#pragma once

#include <QString>

// Shadowlos provisioning.
//
// Shadowlos ships this client as a per-user archive. Rather than patching the
// binary throne.db before download, the archive carries a plaintext
// "shadowlos.json" next to it that names the user's subscription URL. This is
// applied on every launch, so a user who breaks their subscription group gets
// it healed on restart.
namespace Shadowlos {
    // Filename expected in the config directory (alongside throne.db).
    inline constexpr auto BootstrapFileName = "shadowlos.json";

    // Reads the bootstrap file from the current directory and reconciles the
    // managed subscription group: created if missing, URL re-asserted if the
    // user changed it. No-op when the file is absent or malformed, so an
    // ordinary upstream build is unaffected.
    //
    // Must be called after Configs::initDB (needs dataManager).
    void ApplyBootstrap();
}
