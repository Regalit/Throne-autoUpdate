#pragma once

#include "NkrVersion.h"

#include <QString>
#include <QStringList>

class QAbstractButton;
class QMenu;
class QWidget;

// Dev-build error journal, in the Android dev app's format (shadowlos-build
// team/journal-format.md): JSON lines `ts sid seq layer step outcome ms err d` in
// <config dir>/journal/journal.jsonl, capped at 2 MiB, every string scrubbed before it is
// written. Sent to our POST /api/mobile/client-logs (Sl-Sub auth) every 3 minutes; a crash
// of the previous run is reported once on the next start.
//
// Only a dev build (SHADOWLOS_DEV) has any of it. In a prod build every hook below is an
// empty inline function, so upstream-owned files can call them unconditionally.
//
// The user can turn sending off in Tools («Отправлять журнал ошибок разработчикам»,
// settings->shadowlos_send_journal); off clears the journal, the queue and the crash record.
namespace Shadowlos::Journal {

#if SHADOWLOS_DEV

    // `--journal-selftest`: runs the scrub table, prints PASS/FAIL, returns the exit code.
    // -1 when the flag is absent.
    int RunSelfTestIfAsked(const QStringList &args);

    // Opens the journal and writes the start line. Call once, after Logging::Init, with
    // the config dir (where throne.db is) as the current directory. A previous run that
    // did not shut down cleanly is queued as a crash report first.
    void Start();

    // The Tools menu action, the UI tracker (dialogs, taps, warnings), lifecycle, the mode
    // toggles and the upload timer. Call once from Chrome::Install.
    void Install(QWidget *window, QMenu *tools, QAbstractButton *tun, QAbstractButton *systemProxy);

    // One line. layer/step/outcome are fixed words; d and err are scrubbed here.
    // ms < 0 leaves the key out. Thread-safe.
    void Event(const char *layer, const char *step, const char *outcome,
               const QString &d = {}, const QString &err = {}, qint64 ms = -1);

    // Text on its way to the log view (MW_show_log): core and GUI log lines. Warnings and
    // errors are kept; info only within 30 s after a connect starts. Rate capped. Thread-safe.
    void CoreLog(const QString &text);

    // profile_start / profile_stop. Thread-safe.
    void ConnectStart(const QString &server, bool automatic);
    void ConnectEnd(bool ok);
    void Disconnect(bool crash, bool manual);

    // The scrub rules (a port of the Android app's scrub.dart).
    QString Scrub(const QString &text);

    // A journal line with every string value scrubbed, still valid JSON; a line that is
    // not a JSON object is scrubbed as text.
    QString ScrubJsonLine(const QString &line);

#else

    inline int RunSelfTestIfAsked(const QStringList &) { return -1; }
    inline void Start() {}
    inline void Install(QWidget *, QMenu *, QAbstractButton *, QAbstractButton *) {}
    inline void Event(const char *, const char *, const char *,
                      const QString & = {}, const QString & = {}, qint64 = -1) {}
    inline void CoreLog(const QString &) {}
    inline void ConnectStart(const QString &, bool) {}
    inline void ConnectEnd(bool) {}
    inline void Disconnect(bool, bool) {}

#endif

} // namespace Shadowlos::Journal
