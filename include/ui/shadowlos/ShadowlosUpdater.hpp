#pragma once

#include <QString>

class QToolButton;
class QWidget;

// ShadowLos Throne self-update. `ThroneCore selfupdate` (core/internal/selfupdate) does
// the network, signature and file work; this side runs it and drives the «Обновить» button.
// Windows only; elsewhere these are no-ops.
namespace Shadowlos::Updater {

    // Checks quietly 30 s after start and then every 6 h (20 min in dev); when a verified
    // update is ready the button gets an accent. The button is always enabled: pressed with
    // an update downloaded it installs it, otherwise it checks now (or joins the check under
    // way), spinning its icon, and downloads and installs what it finds. Pressing it is the
    // only way an update is applied. The outcome goes on the returned status line, which
    // the caller puts next to the button; no dialogs.
    // After an update (a pending marker left by apply), confirms it once the core is up.
    QWidget *Install(QWidget *window, QToolButton *button);

    // «Обновить» pressed: starts `selfupdate apply`, which waits for this process to end;
    // false (nothing started, e.g. UAC declined) means the app must stay open.
    // resumeProfileId (or -1) is started again after the update.
    bool LaunchApply(int resumeProfileId);

    // After an update: the profile that was running before it, once; -1 otherwise.
    int TakeResumeProfile();

} // namespace Shadowlos::Updater
