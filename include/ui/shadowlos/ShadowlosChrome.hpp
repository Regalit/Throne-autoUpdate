#pragma once

#include <QString>
#include <QtGlobal>

class QMainWindow;
namespace Ui {
    class MainWindow;
}

// Shadowlos desktop look.
//
// Implements the "Windows" page of the "Shadolos APK" Figma file. It has two halves:
//
//  - The "Shadowlos" theme (palette in ThemeManager, sheet in res/shadowlos/shadowlos.qss).
//    It paints every window, dialog, menu and form, so nothing here has to touch
//    the upstream .ui files.
//  - Install(), which rebuilds the main window's top area into the design's cards:
//    a menu card, then power / actions / debug / modes cards, the profile and log tabs,
//    and a status card. It only re-parents widgets uic already created, so
//    mainwindow.ui stays upstream's and every existing connection keeps working.
//
// Kept in our own files so an upstream bump only has to re-apply a few one-line hooks
// (listed in FORK.md).
namespace Shadowlos::Chrome {
    // As listed in Settings -> Theme and stored in settings.
    inline constexpr auto ThemeName = "Shadowlos";

    bool IsThemeName(const QString &theme);

    // True while the Shadowlos theme is the installed one.
    bool ThemeActive();

    // Called by ThemeManager::ApplyTheme before it installs a sheet. Loads PT Root UI once
    // and puts it first in the application font's family list while the theme is active.
    void ApplyThemeFont(bool active);

    // Rebuilds the main window. Call once, straight after ui->setupUi().
    void Install(QMainWindow *window, Ui::MainWindow *ui);

    // Corner labels in the status card: the app version at its left end and, at its right
    // end, how long the running profile has been connected (mm:ss, then h:mm:ss; hidden
    // while nothing runs). Call with true once a profile has started, false when it stops.
    // UI thread only.
    void ConnectStopwatch(bool connected);
    QString StopwatchText(qint64 seconds);
    // For `--journal-selftest` (dev builds): the format and the timer on a throwaway card.
    // Returns the problems found, empty when all is well.
    QString StopwatchSelfTest();

    // The status card is a single line; the texts it shows are written for two.
    QString StatusLine(const QString &text);
}
