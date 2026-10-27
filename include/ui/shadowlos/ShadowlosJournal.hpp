#pragma once

#include "NkrVersion.h"

#include <QJsonObject>
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

    // Text on its way to the log view (MW_show_log): core and GUI log lines. Errors (fail) and
    // warnings (ok) are kept; info only within 30 s after a connect starts. Rate capped. Thread-safe.
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

    // The upload body (client-logs, Windows shape: platform/os, no android/sdk), with the
    // latest geo object when there is one.
    QJsonObject UploadEnvelope(const QString &reason, const QString &requestId, const QString &log);

    // ── geo (ShadowlosGeo.cpp; journal-format.md "Addendum: net step and geo") ──

    // The geo object as the server accepts it: only its keys (net_type carrier mcc_mnc country
    // region asn org; never city), shaped fields checked, free text plain (no IP, host, link, control
    // character), an unsendable org replaced by "AS<asn>". Anything else is left out.
    QJsonObject SanitizeGeo(const QJsonObject &geo);

    // The latest known geo object (net_type at least once checked); empty before. Thread-safe.
    QJsonObject LatestGeo();
    void SetLatestGeoForTest(const QJsonObject &geo);

    // The network-key rules (gateway MAC missing, appearing, changing); "" when all hold.
    QString NetworkKeySelfTest();

    // The 15 s network check, whereami and the start net line. Called from Install.
    void InstallGeo(QObject *owner);

    // A net line for a connect start. UI thread (ConnectStart queues it).
    void GeoConnectStart();

    // How many connects have started this run, and whether one is being brought up or is up
    // (false again after a stop or a core crash). Thread-safe.
    int ConnectCount();
    bool TunnelActive();

    // An app/net line (outcome ok) carrying the nested geo object. Thread-safe.
    void NetEvent(const QString &d, const QJsonObject &geo);

    // The network the last 15 s check saw: its key ("" for none), net_type, and whether a
    // check has run yet; [checks] counts the looks taken (the 15 s check and a connect's).
    // UI thread.
    QByteArray CurrentNetworkKey(QString *netType = nullptr, bool *checked = nullptr, int *checks = nullptr);

    // The network's key read now from the adapters (the 15 s check may not have run since a
    // switch), with the same key rules. It goes through the shared key memo (last adapter,
    // gateway MAC and key) exactly as the check does, so the check then derives the same key;
    // it leaves the check's last network and its counter alone. UI thread.
    QByteArray FreshNetworkKey(QString *netType = nullptr);

    // Whether our TUN adapter (throne-tun, Wintun) is still up: a torn-down tunnel takes a
    // moment to leave. UI thread.
    bool TunAdapterUp();

    // ── the blocking census and its badge (ShadowlosCensus.cpp) ──

    // The badge for the menu card: the blocking type found by the census (the Android app's
    // control / foreign / resilient probe, run off the VPN) plus region and operator from geo.
    // nullptr in a prod build.
    QWidget *MakeBlockingBadge(QWidget *parent);

    // The verdict rules, the kept-verdict rules and the badge text; "" when all hold.
    QString CensusSelfTest();

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
    inline QWidget *MakeBlockingBadge(QWidget *) { return nullptr; }

#endif

} // namespace Shadowlos::Journal
