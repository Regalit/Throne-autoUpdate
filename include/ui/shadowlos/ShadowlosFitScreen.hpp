#pragma once

class QWidget;

// Keeps dialogs inside the screen.
//
// Upstream's dialogs are laid out for a large monitor: «Профиль маршрута» needs about 830 px
// of height, the settings and profile editors 650 to 850. On a 1366x768 laptop at 125% scaling
// Windows leaves about 570 px, so the bottom of such a dialog, OK and Cancel included, ends
// up below the screen with no way to reach it.
//
// Called for every window that is about to be shown. A dialog that fits is left exactly as
// it was. One that does not fit is capped at the screen's free area; if its layout cannot
// shrink that far, its contents move into a scroll area and its OK / Cancel row stays pinned
// under it. Message boxes and file dialogs are left alone (Qt sizes those itself).
namespace Shadowlos::FitScreen {
    void BeforeShow(QWidget *window);
}
