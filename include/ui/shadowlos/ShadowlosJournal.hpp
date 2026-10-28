#pragma once

#include "NkrVersion.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

class QAbstractButton;
class QAction;
class QMenu;
class QObject;
class QWidget;

// Two layers, one namespace.
//
// BOTH builds (prod and dev): the «Улучшение обхода» consent (settings->shadowlos_send_journal,
// the bottom-bar switch), the connect state, geo (whereami, network type), the blocking census
// and its badge, and the anonymous daily summary (ShadowlosTelemetry.cpp, POST
// /api/mobile/telemetry: counters only, a weekly install id, one upload a UTC day).
//
// DEV only (SHADOWLOS_DEV): the error journal in the Android dev app's format (shadowlos-build
// team/journal-format.md): JSON lines under <config dir>/journal, uploaded every 3 minutes,
// crash and manual reports, and the «Улучшение обхода блокировок» page. In a prod build those
// hooks are empty inline functions, so upstream-owned files can call them unconditionally,
// and none of that code is compiled.
namespace Shadowlos::Journal {

    // ── both builds ──────────────────────────────────────────────────────────

    // The consent, geo, the census tick and the daily summary; in dev also the journal (its
    // Tools menu action, UI tracker and upload timer). Call once from Chrome::Install.
    void Install(QWidget *window, QMenu *tools, QAbstractButton *tun, QAbstractButton *systemProxy);

    // The «Улучшение обхода» consent: the bottom-bar switch and Продвинутый's toggle flip this
    // action; nullptr before Install.
    QAction *ConsentAction();
    bool ConsentOn();

    // profile_start / profile_stop. Thread-safe.
    void ConnectStart(const QString &server, bool automatic);
    void ConnectEnd(bool ok);
    void Disconnect(bool crash, bool manual);

    // How many connects have started this run, and whether one is being brought up or is up
    // (false again after a stop or a core crash). Thread-safe.
    int ConnectCount();
    bool TunnelActive();
    bool Connecting();

    // The scrub rules (a port of the Android app's scrub.dart); geo's free text must pass them.
    QString Scrub(const QString &text);

    // ── geo (ShadowlosGeo.cpp) ──

    // The geo object as the server accepts it: only its keys (net_type carrier mcc_mnc country
    // region asn org; never city), shaped fields checked, free text plain (no IP, host, link, control
    // character), an unsendable org replaced by "AS<asn>". Anything else is left out.
    QJsonObject SanitizeGeo(const QJsonObject &geo);

    // The latest known geo object (net_type at least once checked); empty before. Thread-safe.
    QJsonObject LatestGeo();
    void SetLatestGeoForTest(const QJsonObject &geo);

    // The network-key rules (gateway MAC missing, appearing, changing); "" when all hold.
    QString NetworkKeySelfTest();

    // The 15 s network check and whereami. Called from Install.
    void InstallGeo(QObject *owner);

    // A look at the network for a connect start (and, in dev, its net line). UI thread.
    void GeoConnectStart();

    // The network the last 15 s check saw: its key ("" for none), net_type, and whether a
    // check has run yet; [checks] counts the looks taken (the 15 s check and a connect's).
    // UI thread.
    QByteArray CurrentNetworkKey(QString *netType = nullptr, bool *checked = nullptr, int *checks = nullptr);

    // The network's key read now from the adapters (the 15 s check may not have run since a
    // switch), with the same key rules. UI thread.
    QByteArray FreshNetworkKey(QString *netType = nullptr);

    // Whether our TUN adapter (throne-tun, Wintun) is still up: a torn-down tunnel takes a
    // moment to leave. UI thread.
    bool TunAdapterUp();

    // ── the blocking census and its badge (ShadowlosCensus.cpp) ──

    // The badge for the bottom bar: the blocking type found by the census (the Android app's
    // control / foreign / resilient probe, run off the VPN) plus region and operator from geo.
    QWidget *MakeBlockingBadge(QWidget *parent);

    // The verdict rules, the kept-verdict rules and the badge text; "" when all hold.
    QString CensusSelfTest();

    // The verdict shown for the current network in the telemetry contract's words: none,
    // regular, whitelist, noInternet or unknown. measured is false before anything was
    // measured on this network. UI thread.
    QString CensusBlockingWord(bool *measured = nullptr);

    // A census result. measured is false when no verdict came (the tunnel is up, the network
    // changed meanwhile); note then says why.
    struct CensusNow {
        bool measured = false;
        QString verdict; // «Нет блокировок», «Обычные блокировки», …; «Не удалось определить»
        QString counts;  // «контроль 2/2, иностранные 0/5, устойчивые 5/5»
        QString region;
        QString op;
        qint64 at = 0; // ms since epoch
        QString note;
    };

    // Measures now (a pass is joined when one is under way); done runs on the UI thread once.
    // True when it first waits for a torn-down tunnel to go (the census tick then runs it).
    bool CensusMeasureNow(std::function<void(const CensusNow &)> done);

    // The last result on the current network; false before anything was measured on it.
    bool CensusLast(CensusNow *out);

    // Whether a probe may go out now on the user's own network: not while our tunnel is up in
    // TUN mode, nor while a torn-down tunnel is still leaving. UI thread.
    bool OffVpnNow();

    // ── the daily summary (ShadowlosTelemetry.cpp) ──

    // The summary's timer, started from Install.
    void InstallTelemetry(QObject *owner);

    // The pure rules (probe classification, the body against the server contract, the weekly
    // id); "" when all hold.
    QString TelemetrySelfTest();

#if SHADOWLOS_DEV

    // ── dev only: the journal (ShadowlosJournal.cpp, ShadowlosScrub.cpp) ──

    // `--journal-selftest`: runs the scrub table and the other rules, prints PASS/FAIL, returns
    // the exit code. -1 when the flag is absent.
    int RunSelfTestIfAsked(const QStringList &args);

    // Opens the journal and writes the start line. Call once, after Logging::Init, with
    // the config dir (where throne.db is) as the current directory. A previous run that
    // did not shut down cleanly is queued as a crash report first.
    void Start();

    // The journal's part of Install: the consent action goes into Tools, the UI tracker,
    // lifecycle, the mode toggles and the upload timer.
    void InstallJournal(QWidget *window, QMenu *tools, QAction *consent, QAbstractButton *tun, QAbstractButton *systemProxy);

    // The journal's side of the consent: off clears the journal, the queue and the crash record.
    void JournalConsentChanged(bool on);

    // The journal's lines for the connect hooks (the shared state is kept by the callers).
    void JournalConnectStart(const QString &server, bool automatic);
    void JournalConnectEnd(bool ok);
    void JournalDisconnect(bool crash, bool manual);

    // One line. layer/step/outcome are fixed words; d and err are scrubbed here.
    // ms < 0 leaves the key out. Thread-safe.
    void Event(const char *layer, const char *step, const char *outcome,
               const QString &d = {}, const QString &err = {}, qint64 ms = -1);

    // Text on its way to the log view (MW_show_log): core and GUI log lines. Errors (fail) and
    // warnings (ok) are kept; info only within 30 s after a connect starts. Rate capped. Thread-safe.
    void CoreLog(const QString &text);

    // A journal line with every string value scrubbed, still valid JSON; a line that is
    // not a JSON object is scrubbed as text.
    QString ScrubJsonLine(const QString &line);

    // The upload body (Windows shape: platform/os, no android/sdk), with the latest geo object
    // when there is one.
    QJsonObject UploadEnvelope(const QString &reason, const QString &requestId, const QString &log);

    // An app/net line (outcome ok) carrying the nested geo object. Thread-safe.
    void NetEvent(const QString &d, const QJsonObject &geo);

    // ── the «Улучшение обхода блокировок» page (ShadowlosImprove.cpp) ──

    // «Отправить логи»: a reason "manual" upload of the latest journal lines, now. Manual
    // reports share the server's 20 a day per user with crash reports, so the client allows one
    // per 10 min and 5 a UTC day, and waits out a 429's Retry-After (kept across restarts).
    // retryAfter is the 429's Retry-After in seconds (0 when absent).
    enum class ManualResult { Sent, Off, NoToken, NoConnection, TooOften, Refused, Cooldown };
    void SendManualReport(std::function<void(ManualResult result, int status, qint64 retryAfter)> done);

    // Seconds until a manual report may go (0: now) and a short note saying why. UI thread.
    qint64 ManualReportWait(QString *note = nullptr);

    // The cooldown rules; "" when all hold.
    QString ManualQuotaSelfTest();

    // The page and its menu item, appended to [menu]. Call after Install.
    void InstallImprovePage(QWidget *window, QMenu *menu);

#else

    // Prod: no journal, no log upload, no crash or manual report, no page.
    inline int RunSelfTestIfAsked(const QStringList &) { return -1; }
    inline void Start() {}
    inline void Event(const char *, const char *, const char *,
                      const QString & = {}, const QString & = {}, qint64 = -1) {}
    inline void CoreLog(const QString &) {}
    inline void NetEvent(const QString &, const QJsonObject &) {}
    inline void InstallImprovePage(QWidget *, QMenu *) {}

#endif

} // namespace Shadowlos::Journal
