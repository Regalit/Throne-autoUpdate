#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "include/global/ShadowlosBootstrap.hpp"
#include "include/database/entities/Profile.h"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <vector>

// Both builds. The «Улучшение обхода» consent, the connect state, and the anonymous daily
// summary of «Данные о блокировках» (shadowlos mobile-api handlers/telemetry.go,
// deploy/telemetry.md): POST /api/mobile/telemetry once a UTC day while the switch is on.
//
// What it sends, and nothing else: the app id and version, a random install id rotated
// weekly (never the X-Sl-Install-Id the request authenticates with), the UTC day, the Windows
// major version, the network type and the census's blocking verdict, and for each site of
// GET /api/mobile/telemetry/probes.json whether it opened on the direct road with the failure
// class when it did not. Region and provider the server reads from the request IP, which is
// why the upload and the probes go straight from the user's network (never our proxy, never
// while our TUN tunnel is up). No host, no IP, no time of day, no log text.
namespace Shadowlos::Journal {

    // ── consent and the connect state ────────────────────────────────────────

    namespace {

        Configs::SettingsRepo *settings() {
            return Configs::dataManager->settingsRepo.get();
        }

        QPointer<QAction> consentAction;

        struct Conn {
            QMutex mutex;
            int connects = 0; // ConnectStart calls this run (geo drops a whereami answer that spans one)
            bool connecting = false;
            bool up = false;
        };

        Conn &conn() {
            static Conn c;
            return c;
        }

        void scheduleSoon(int ms);

    } // namespace

    // ── the summary's error counts (shadowlos-server team/telemetry-errors-contract.md) ──
    //
    // Per UTC day, in <config dir>/telemetry-errors.json, never in the journal: crashes of the
    // app, other abnormal ends (the core died, a kill, a run that did not exit cleanly), and
    // failed connects as (class, kind, country) counts. No host, IP, server name or id, and no
    // error text is kept: the core's error is reduced to its class at once. Counted only while
    // «Улучшение обхода» is on.

    namespace {

        constexpr int kMaxConnectRows = 100;
        constexpr int kMaxErrorCount = 10000;
        constexpr int kErrorDaysKept = 15; // the contract's day window (today and 14 back)

        const QRegularExpression &kindRe() {
            static const QRegularExpression re(QStringLiteral("^[a-z0-9_]{1,24}$"));
            return re;
        }

        const QRegularExpression &countryRe() {
            static const QRegularExpression re(QStringLiteral("^[A-Z]{2}$"));
            return re;
        }

        const QStringList &connectClasses() {
            static const QStringList l{"dns", "tcp", "tls", "handshake", "no_rx", "timeout", "other"};
            return l;
        }

        // The core's answer to a start, reduced to a class. No answer in time is a timeout.
        QString connectClass(bool rpcOK, const QString &error) {
            if (!rpcOK) return QStringLiteral("timeout");
            const QString e = error.toLower();
            const auto has = [&e](std::initializer_list<const char *> words) {
                for (const char *w : words) {
                    if (e.contains(QLatin1String(w))) return true;
                }
                return false;
            };
            if (has({"no such host", "lookup ", "dns", "resolve"})) return QStringLiteral("dns");
            if (has({"x509", "certificate", "tls", "reality"})) return QStringLiteral("tls");
            if (has({"handshake"})) return QStringLiteral("handshake");
            if (has({"connection refused", "actively refused", "connection reset", "unreachable", "connectex", "dial tcp"}))
                return QStringLiteral("tcp");
            if (has({"timeout", "timed out", "deadline exceeded"})) return QStringLiteral("timeout");
            return QStringLiteral("other");
        }

        // The protocol / transport label: the profile type, then its transport (or reality),
        // e.g. vless_reality, vless_xhttp, hysteria2, ss, trojan_ws.
        QString kindLabel(const QString &type, const QString &transport, bool reality) {
            static const QRegularExpression notWord(QStringLiteral("[^a-z0-9]"));
            QString base = type.toLower().remove(notWord);
            if (base == QStringLiteral("xrayvless")) base = QStringLiteral("vless");
            else if (base == QStringLiteral("xrayvmess")) base = QStringLiteral("vmess");
            else if (base == QStringLiteral("xraytrojan")) base = QStringLiteral("trojan");
            else if (base == QStringLiteral("shadowsocks")) base = QStringLiteral("ss");
            else if (base == QStringLiteral("wireguard")) base = QStringLiteral("wg");
            QString t = transport.toLower().remove(notWord);
            if (t == QStringLiteral("websocket")) t = QStringLiteral("ws");
            if (t.isEmpty() && reality) t = QStringLiteral("reality");
            if (base.isEmpty()) base = QStringLiteral("other");
            QString kind = t.isEmpty() ? base : base + u'_' + t;
            kind = kind.left(24);
            while (kind.endsWith(u'_')) kind.chop(1);
            return kindRe().match(kind).hasMatch() ? kind : QStringLiteral("other");
        }

        struct DayErrors {
            int crashes = 0;
            int abnormal = 0;
            QMap<QString, int> connect; // "class|kind|country" -> count
            int total() const {
                int n = crashes + abnormal;
                for (const int c : connect) n += c;
                return n;
            }
        };

        // The errors block for one day; empty when every count is zero (then it is left out).
        QJsonObject errorsBlock(const DayErrors &d) {
            QJsonObject out;
            if (d.crashes > 0) out.insert(QStringLiteral("crashes"), qMin(d.crashes, kMaxErrorCount));
            if (d.abnormal > 0) out.insert(QStringLiteral("abnormal_exits"), qMin(d.abnormal, kMaxErrorCount));
            QList<QPair<QString, int>> rows;
            for (auto it = d.connect.cbegin(); it != d.connect.cend(); ++it) {
                if (it.value() > 0) rows << qMakePair(it.key(), it.value());
            }
            // The most frequent first, at most 100.
            std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            QJsonArray connect;
            for (const auto &[key, count] : rows) {
                if (connect.size() >= kMaxConnectRows) break;
                const QStringList parts = key.split(u'|');
                if (parts.size() != 3 || !connectClasses().contains(parts[0]) || !kindRe().match(parts[1]).hasMatch()) continue;
                QJsonObject row{{"class", parts[0]}, {"kind", parts[1]}, {"count", qMin(count, kMaxErrorCount)}};
                if (countryRe().match(parts[2]).hasMatch()) row.insert(QStringLiteral("country"), parts[2]);
                connect.append(row);
            }
            if (!connect.isEmpty()) out.insert(QStringLiteral("connect"), connect);
            return out;
        }

        struct Errs {
            QMutex mutex;
            QString file;
            QMap<QString, DayErrors> days; // by UTC day
            QString kind = QStringLiteral("other"); // the profile being started
            QString country;
            QString cls; // the class of the last start's error, "" for none
        };

        Errs &errs() {
            static Errs e;
            return e;
        }

        QString utcDayNow() {
            return QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
        }

        void loadErrorsLocked() {
            auto &e = errs();
            QFile f(e.file);
            if (!f.open(QIODevice::ReadOnly)) return;
            const QJsonObject days = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("days")).toObject();
            for (auto it = days.begin(); it != days.end(); ++it) {
                const QJsonObject o = it.value().toObject();
                DayErrors d;
                d.crashes = o.value(QStringLiteral("crashes")).toInt();
                d.abnormal = o.value(QStringLiteral("abnormal_exits")).toInt();
                const QJsonObject c = o.value(QStringLiteral("connect")).toObject();
                for (auto ci = c.begin(); ci != c.end(); ++ci) d.connect.insert(ci.key(), ci.value().toInt());
                e.days.insert(it.key(), d);
            }
        }

        void saveErrorsLocked() {
            auto &e = errs();
            if (e.file.isEmpty()) return;
            while (e.days.size() > kErrorDaysKept) e.days.erase(e.days.begin()); // the oldest day first
            QJsonObject days;
            for (auto it = e.days.cbegin(); it != e.days.cend(); ++it) {
                QJsonObject c;
                for (auto ci = it->connect.cbegin(); ci != it->connect.cend(); ++ci) c.insert(ci.key(), ci.value());
                days.insert(it.key(), QJsonObject{{"crashes", it->crashes}, {"abnormal_exits", it->abnormal}, {"connect", c}});
            }
            QSaveFile f(e.file);
            if (!f.open(QIODevice::WriteOnly)) return;
            f.write(QJsonDocument(QJsonObject{{"days", days}}).toJson(QJsonDocument::Compact));
            f.commit();
        }

        enum class ErrorKind { Crash, Abnormal, Connect };

        // Thread-safe. Nothing is counted while the switch is off.
        void countError(ErrorKind kind, const QString &cls = {}) {
            if (!ConsentOn()) return;
            auto &e = errs();
            QMutexLocker lock(&e.mutex);
            DayErrors &d = e.days[utcDayNow()];
            switch (kind) {
                case ErrorKind::Crash: d.crashes = qMin(d.crashes + 1, kMaxErrorCount); break;
                case ErrorKind::Abnormal: d.abnormal = qMin(d.abnormal + 1, kMaxErrorCount); break;
                case ErrorKind::Connect: {
                    const QString key = cls + u'|' + e.kind + u'|' + e.country;
                    // A new row only while there is room for it; known rows keep counting.
                    if (!d.connect.contains(key) && d.connect.size() >= 4 * kMaxConnectRows) return;
                    d.connect[key] = qMin(d.connect.value(key) + 1, kMaxErrorCount);
                    break;
                }
            }
            saveErrorsLocked();
        }

        // The errors block of [day] (empty: leave it out) and its total.
        QJsonObject errorsFor(const QString &day, int *total = nullptr) {
            auto &e = errs();
            QMutexLocker lock(&e.mutex);
            const DayErrors d = e.days.value(day);
            if (total != nullptr) *total = d.total();
            return errorsBlock(d);
        }

        // <config dir>/telemetry-run.marker: written at start, removed on a clean exit. Found at
        // start, the previous run did not end cleanly: a crash when the crash handler left a
        // report after it began, else an abnormal exit (killed, power loss).
        void countPreviousRun(QObject *owner) {
            const QString marker = QDir::current().absoluteFilePath(QStringLiteral("telemetry-run.marker"));
            const QFileInfo previous(marker);
            if (previous.exists()) {
                const QDateTime began = previous.lastModified();
                const auto reports = QDir(Logging::CrashDir()).entryInfoList(QDir::Files, QDir::Time);
                const bool crash = !reports.isEmpty() && reports.first().lastModified() >= began;
                countError(crash ? ErrorKind::Crash : ErrorKind::Abnormal);
            }
            QFile f(marker);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write("run\n"); // the time is the file's
            QObject::connect(qApp, &QCoreApplication::aboutToQuit, owner, [marker] { QFile::remove(marker); });
        }

    } // namespace

    void ConnectProfile(const std::shared_ptr<Configs::Profile> &ent) {
        QString kind = QStringLiteral("other"), country;
        if (ent && ent->outbound) {
            const auto security = ent->outbound->GetSecurity();
            kind = kindLabel(ent->type, security.transport, security.label == QObject::tr("Reality"));
            if (countryRe().match(ent->test_country).hasMatch()) country = ent->test_country;
        }
        auto &e = errs();
        QMutexLocker lock(&e.mutex);
        e.kind = kind;
        e.country = country;
        e.cls.clear();
    }

    void ConnectResult(bool rpcOK, const QString &error) {
        if (rpcOK && error.isEmpty()) return;
        const QString cls = connectClass(rpcOK, error); // the text goes no further
        auto &e = errs();
        QMutexLocker lock(&e.mutex);
        e.cls = cls;
    }

    bool ConsentOn() {
        return settings()->shadowlos_consent == 1 && settings()->shadowlos_send_journal != 0;
    }

    QAction *ConsentAction() {
        return consentAction;
    }

    void ConnectStart(const QString &server, bool automatic) {
        {
            QMutexLocker lock(&conn().mutex);
            conn().connecting = true;
            ++conn().connects;
        }
#if SHADOWLOS_DEV
        JournalConnectStart(server, automatic);
#else
        Q_UNUSED(server)
        Q_UNUSED(automatic)
#endif
        // Geo looks from the UI thread; this runs on a worker.
        if (qApp) QMetaObject::invokeMethod(qApp, [] { GeoConnectStart(); }, Qt::QueuedConnection);
    }

    void ConnectEnd(bool ok) {
        {
            QMutexLocker lock(&conn().mutex);
            conn().connecting = false;
            if (ok) conn().up = true;
        }
        if (!ok) {
            QString cls;
            {
                QMutexLocker lock(&errs().mutex);
                cls = errs().cls.isEmpty() ? QStringLiteral("other") : errs().cls;
            }
            countError(ErrorKind::Connect, cls);
        }
#if SHADOWLOS_DEV
        JournalConnectEnd(ok);
#endif
    }

    void Disconnect(bool crash, bool manual) {
        {
            QMutexLocker lock(&conn().mutex);
            conn().up = false;
        }
        // The core died under a running profile (Process.cpp); a «Reset» of the core is manual.
        if (crash && !manual) countError(ErrorKind::Abnormal);
#if SHADOWLOS_DEV
        JournalDisconnect(crash, manual);
#else
        Q_UNUSED(crash)
        Q_UNUSED(manual)
#endif
    }

    int ConnectCount() {
        QMutexLocker lock(&conn().mutex);
        return conn().connects;
    }

    // From the profile_start/profile_stop hooks: a core crash goes through profile_stop too,
    // which settings->started_id does not follow.
    bool TunnelActive() {
        QMutexLocker lock(&conn().mutex);
        return conn().connecting || conn().up;
    }

    bool Connecting() {
        QMutexLocker lock(&conn().mutex);
        return conn().connecting;
    }

    void Install(QWidget *window, QMenu *tools, QAbstractButton *tun, QAbstractButton *systemProxy) {
        auto *action = new QAction(QStringLiteral("Улучшение обхода: отправлять анонимную сводку о блокировках"), window);
        action->setCheckable(true);
        action->setChecked(ConsentOn());
        QObject::connect(action, &QAction::toggled, window, [](bool on) {
            settings()->shadowlos_send_journal = on ? 1 : 0;
            // Turning it on is consent, whatever the window was answered before.
            if (on) settings()->shadowlos_consent = 1;
            settings()->Save();
#if SHADOWLOS_DEV
            JournalConsentChanged(on);
#endif
            LOG_INFO(QStringLiteral("[telemetry] «Улучшение обхода» turned %1").arg(on ? "on" : "off"));
            if (on) scheduleSoon(30 * 1000);
        });
        consentAction = action;
        InstallConsent(window, action);
#if SHADOWLOS_DEV
        InstallJournal(window, tools, action, tun, systemProxy);
#else
        Q_UNUSED(tools)
        Q_UNUSED(tun)
        Q_UNUSED(systemProxy)
#endif
        InstallGeo(window);
        {
            QMutexLocker lock(&errs().mutex);
            errs().file = QDir::current().absoluteFilePath(QStringLiteral("telemetry-errors.json"));
            loadErrorsLocked();
        }
        countPreviousRun(window);
        InstallTelemetry(window);
    }

    // ── the daily summary ────────────────────────────────────────────────────

    namespace {

        constexpr auto kTelemetryPath = "/api/mobile/telemetry";
        constexpr auto kProbesPath = "/api/mobile/telemetry/probes.json";
        constexpr auto kAppId = SHADOWLOS_DEV ? "shadowlos-throne-dev" : "shadowlos-throne";
        constexpr int kFirstLookMs = 90 * 1000;       // a look 90 s after start (the day's delay still applies)
        constexpr int kLookEveryMs = 10 * 60 * 1000;
        constexpr qint64 kBackoffFirstSecs = 30 * 60; // 30, 60, 120, 240 min, then 6 h
        constexpr qint64 kBackoffMaxSecs = 6 * 60 * 60;
        constexpr qint64 kServerHoldSecs = 60 * 60;   // a 429/503 without Retry-After
        constexpr qint64 kStaleHoldSecs = 5 * 60;     // the network or the tunnel changed during a pass
        constexpr qint64 kDayDelayMaxSecs = 60 * 60;  // the random delay before a day's first attempt
        constexpr int kParallelProbes = 4;
        constexpr int kWaitCensusLooks = 3; // looks without a verdict before «unknown» is sent
        constexpr int kMaxSites = 200;      // the server's listed limit
        constexpr int kMaxBodyBytes = 16 * 1024;
        constexpr auto kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";

        // ── pure: the site list, a probe's answer, its class ─────────────────

        struct Site {
            QString id;
            QUrl url;
            QList<int> okStatus;
            QJsonArray markers;
            bool verifyTls = true;
            bool cookies = false;
            int redirects = 5;
            int timeoutMs = 4000;
            int maxBody = 16384;
        };

        struct Answer {
            int status = 0;               // the final HTTP status; 0 when none came
            QNetworkReply::NetworkError error = QNetworkReply::NoError;
            bool timedOut = false;
            bool tlsFailed = false;
            QByteArray body;              // at most maxBody bytes
            QList<QNetworkReply::RawHeaderPair> headers;
            QStringList redirects;        // every redirect target seen
        };

        struct Outcome {
            bool works = false;
            QString fail; // a key of the server's fails set, or "" for none
        };

        const QRegularExpression &siteIdRe() {
            static const QRegularExpression re(QStringLiteral("^[a-z0-9_.-]{1,64}$"));
            return re;
        }

        // probes.json: probe_defaults under each site's probe; sites without an https URL or a
        // valid id are left out, and so is a second site with the same id.
        std::vector<Site> parseSites(const QJsonObject &doc) {
            std::vector<Site> out;
            const QJsonObject defaults = doc.value(QStringLiteral("probe_defaults")).toObject();
            QSet<QString> seen;
            for (const QJsonValue &v : doc.value(QStringLiteral("sites")).toArray()) {
                const QJsonObject o = v.toObject();
                QJsonObject probe = defaults;
                const QJsonObject own = o.value(QStringLiteral("probe")).toObject();
                for (auto it = own.begin(); it != own.end(); ++it) probe.insert(it.key(), it.value());
                Site s;
                s.id = o.value(QStringLiteral("id")).toString();
                s.url = QUrl(probe.value(QStringLiteral("url")).toString());
                if (!siteIdRe().match(s.id).hasMatch() || seen.contains(s.id) || !s.url.isValid() ||
                    s.url.scheme() != QStringLiteral("https") || s.url.host().isEmpty()) continue;
                seen.insert(s.id);
                for (const QJsonValue &st : o.value(QStringLiteral("ok_status")).toArray()) s.okStatus << st.toInt();
                if (s.okStatus.isEmpty()) s.okStatus << 200;
                s.markers = o.value(QStringLiteral("markers")).toArray();
                s.verifyTls = probe.value(QStringLiteral("verify_tls")).toBool(true);
                s.cookies = probe.value(QStringLiteral("cookies")).toBool(false);
                s.redirects = qBound(0, probe.value(QStringLiteral("follow_redirects")).toInt(5), 10);
                s.timeoutMs = qBound(1000, probe.value(QStringLiteral("timeout_ms")).toInt(4000), 15000);
                s.maxBody = qBound(1024, probe.value(QStringLiteral("max_body_bytes")).toInt(16384), 256 * 1024);
                out.push_back(std::move(s));
                if (int(out.size()) >= kMaxSites) break;
            }
            return out;
        }

        QString titleOf(const QByteArray &body) {
            static const QRegularExpression re(QStringLiteral("<title[^>]*>(.*?)</title>"),
                                               QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
            const auto m = re.match(QString::fromUtf8(body));
            return m.hasMatch() ? m.captured(1).simplified() : QString();
        }

        // Every field a marker names must hold (match_fields: status, title_contains,
        // body_contains, header, redirect_contains).
        bool markerMatches(const QJsonObject &m, const Answer &a) {
            bool any = false;
            if (m.contains(QStringLiteral("status"))) {
                any = true;
                if (a.status != m.value(QStringLiteral("status")).toInt()) return false;
            }
            if (m.contains(QStringLiteral("title_contains"))) {
                any = true;
                if (!titleOf(a.body).contains(m.value(QStringLiteral("title_contains")).toString(), Qt::CaseInsensitive)) return false;
            }
            if (m.contains(QStringLiteral("body_contains"))) {
                any = true;
                if (!QString::fromUtf8(a.body).contains(m.value(QStringLiteral("body_contains")).toString())) return false;
            }
            if (m.contains(QStringLiteral("header"))) {
                any = true;
                const QJsonObject h = m.value(QStringLiteral("header")).toObject();
                const QByteArray name = h.value(QStringLiteral("name")).toString().toLatin1().toLower();
                const QString want = h.value(QStringLiteral("contains")).toString();
                bool found = false;
                for (const auto &pair : a.headers) {
                    if (pair.first.toLower() == name && QString::fromLatin1(pair.second).contains(want, Qt::CaseInsensitive)) found = true;
                }
                if (!found) return false;
            }
            if (m.contains(QStringLiteral("redirect_contains"))) {
                any = true;
                const QString want = m.value(QStringLiteral("redirect_contains")).toString();
                bool found = false;
                for (const auto &r : a.redirects) found = found || r.contains(want);
                if (!found) return false;
            }
            return any;
        }

        // The probes.json classes in the server's fails keys; http_429 and geo_block have none
        // (works false, no counter).
        QString failKey(const QString &cls) {
            if (cls == QStringLiteral("captcha")) return QStringLiteral("captcha");
            if (cls == QStringLiteral("block_page")) return QStringLiteral("blockpage");
            if (cls == QStringLiteral("http_403")) return QStringLiteral("http403");
            if (cls == QStringLiteral("http_451")) return QStringLiteral("http451");
            return {};
        }

        Outcome classify(const Site &site, const Answer &a) {
            if (a.status > 0) {
                for (const QJsonValue &v : site.markers) {
                    const QJsonObject m = v.toObject();
                    if (m.value(QStringLiteral("class")).toString() == QStringLiteral("ok")) continue;
                    if (markerMatches(m, a)) return {false, failKey(m.value(QStringLiteral("class")).toString())};
                }
                if (site.okStatus.contains(a.status)) return {true, {}};
                if (a.status == 403) return {false, QStringLiteral("http403")};
                if (a.status == 451) return {false, QStringLiteral("http451")};
                return {false, {}};
            }
            if (a.timedOut) return {false, QStringLiteral("timeout")};
            if (a.tlsFailed) return {false, QStringLiteral("tls")};
            switch (a.error) {
                case QNetworkReply::TimeoutError:
                case QNetworkReply::OperationCanceledError: return {false, QStringLiteral("timeout")};
                case QNetworkReply::ConnectionRefusedError: return {false, QStringLiteral("refused")};
                case QNetworkReply::RemoteHostClosedError: return {false, QStringLiteral("reset")};
                case QNetworkReply::SslHandshakeFailedError: return {false, QStringLiteral("tls")};
                default: return {false, {}}; // a name failure among them: no key for it
            }
        }

        // ── pure: the body and its check against the server contract ─────────

        int osMajor() {
#ifdef Q_OS_WIN
            return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11 ? 11 : 10;
#else
            return 0;
#endif
        }

        // The server's net types; our geo says ethernet, wifi, other or none.
        QString netTypeWord(const QString &geoType) {
            if (geoType == QStringLiteral("wifi") || geoType == QStringLiteral("ethernet")) return geoType;
            return QStringLiteral("other");
        }

        QJsonObject buildBody(const QString &install, const QString &day, const QString &netType, const QString &blocking,
                              const QList<QPair<QString, Outcome>> &rows) {
            QJsonArray listed;
            for (const auto &[id, o] : rows) {
                QJsonObject fails;
                if (!o.fail.isEmpty()) fails.insert(o.fail, 1);
                listed.append(QJsonObject{{"site", id}, {"road", "direct"}, {"works", o.works}, {"fails", fails}});
            }
            QJsonObject body{
                {"schema", 1},
                {"app_id", QLatin1String(kAppId)},
                {"app_version", QStringLiteral(NKR_VERSION)},
                {"install", install},
                {"day", day},
                {"kind", "daily"},
                {"net", QJsonObject{{"type", netTypeWord(netType)}, {"blocking", blocking}}},
                {"probe", QJsonObject{{"ran", !rows.isEmpty()}}},
            };
            if (const int os = osMajor(); os > 0) body.insert(QStringLiteral("os_major"), os);
            if (!listed.isEmpty()) body.insert(QStringLiteral("listed"), listed);
            return body;
        }

        const QRegularExpression &uuidV4Re() {
            static const QRegularExpression re(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
            return re;
        }

        // The server's validate() for what we send (handlers/telemetry.go): "" when it would
        // take it. Unknown keys anywhere are a 400 there, so they are checked too.
        QString contractProblem(const QJsonObject &b, const QString &authInstall, const QDate &today) {
            static const QSet<QString> top{"schema", "app_id", "app_version", "install", "day", "android_major", "os_major",
                                           "kind", "net", "listed", "unlisted", "probe", "errors"};
            static const QSet<QString> failKeys{"timeout", "reset", "refused", "tls", "blockpage", "captcha", "http403", "http451"};
            static const QSet<QString> blockings{"none", "regular", "whitelist", "noInternet", "unknown"};
            static const QSet<QString> netTypes{"wifi", "cell", "ethernet", "other"};
            for (const QString &k : b.keys()) {
                if (!top.contains(k)) return QStringLiteral("unknown key ") + k;
            }
            if (b.value(QStringLiteral("schema")).toInt() != 1) return QStringLiteral("bad schema");
            const QString app = b.value(QStringLiteral("app_id")).toString();
            if (app != QLatin1String(kAppId)) return QStringLiteral("bad app_id"); // this build's own id
            const QString version = b.value(QStringLiteral("app_version")).toString();
            if (version.isEmpty() || version.toUtf8().size() > 32) return QStringLiteral("bad app_version");
            const QString install = b.value(QStringLiteral("install")).toString();
            if (!uuidV4Re().match(install).hasMatch() || install == authInstall) return QStringLiteral("bad install");
            const QDate day = QDate::fromString(b.value(QStringLiteral("day")).toString(), Qt::ISODate);
            if (!day.isValid() || day > today || day < today.addDays(-14)) return QStringLiteral("bad day");
            if (b.value(QStringLiteral("kind")).toString() != QStringLiteral("daily")) return QStringLiteral("bad kind");
            if (b.contains(QStringLiteral("android_major"))) return QStringLiteral("bad android_major");
            if (const int os = b.value(QStringLiteral("os_major")).toInt(); b.contains(QStringLiteral("os_major")) && (os < 1 || os > 99))
                return QStringLiteral("bad os_major");
            const QJsonObject net = b.value(QStringLiteral("net")).toObject();
            for (const QString &k : net.keys()) {
                if (k != QStringLiteral("type") && k != QStringLiteral("blocking")) return QStringLiteral("bad net key ") + k;
            }
            if (!netTypes.contains(net.value(QStringLiteral("type")).toString()) || !blockings.contains(net.value(QStringLiteral("blocking")).toString()))
                return QStringLiteral("bad net");
            const QJsonObject probe = b.value(QStringLiteral("probe")).toObject();
            for (const QString &k : probe.keys()) {
                if (k != QStringLiteral("ran")) return QStringLiteral("bad probe key ") + k;
            }
            const QJsonArray listed = b.value(QStringLiteral("listed")).toArray();
            if (listed.size() > kMaxSites) return QStringLiteral("too many listed");
            QSet<QString> seen;
            for (const QJsonValue &v : listed) {
                const QJsonObject row = v.toObject();
                for (const QString &k : row.keys()) {
                    if (k != QStringLiteral("site") && k != QStringLiteral("road") && k != QStringLiteral("works") && k != QStringLiteral("fails"))
                        return QStringLiteral("bad listed key ") + k;
                }
                const QString site = row.value(QStringLiteral("site")).toString();
                const QString road = row.value(QStringLiteral("road")).toString();
                if (!siteIdRe().match(site).hasMatch() || road != QStringLiteral("direct") || !row.value(QStringLiteral("works")).isBool() ||
                    seen.contains(site + u' ' + road))
                    return QStringLiteral("bad listed");
                seen.insert(site + u' ' + road);
                const QJsonObject fails = row.value(QStringLiteral("fails")).toObject();
                for (auto it = fails.begin(); it != fails.end(); ++it) {
                    if (!failKeys.contains(it.key()) || it.value().toInt(-1) < 0 || it.value().toInt() > 10000) return QStringLiteral("bad fails");
                }
            }
            if (b.contains(QStringLiteral("unlisted"))) return QStringLiteral("unlisted is never sent");
            if (b.contains(QStringLiteral("errors"))) {
                const QJsonObject e = b.value(QStringLiteral("errors")).toObject();
                if (e.isEmpty()) return QStringLiteral("empty errors");
                for (const QString &k : e.keys()) {
                    if (k != QStringLiteral("crashes") && k != QStringLiteral("abnormal_exits") && k != QStringLiteral("connect"))
                        return QStringLiteral("bad errors key ") + k;
                }
                for (const char *k : {"crashes", "abnormal_exits"}) {
                    if (e.contains(QLatin1String(k)) && (e.value(QLatin1String(k)).toInt(-1) < 1 || e.value(QLatin1String(k)).toInt() > 10000))
                        return QStringLiteral("bad errors count");
                }
                const QJsonArray rows = e.value(QStringLiteral("connect")).toArray();
                if (rows.size() > 100 || (e.contains(QStringLiteral("connect")) && rows.isEmpty())) return QStringLiteral("bad connect rows");
                QSet<QString> keys;
                for (const QJsonValue &v : rows) {
                    const QJsonObject r = v.toObject();
                    for (const QString &k : r.keys()) {
                        if (k != QStringLiteral("class") && k != QStringLiteral("kind") && k != QStringLiteral("country") && k != QStringLiteral("count"))
                            return QStringLiteral("bad connect key ") + k;
                    }
                    const QString cls = r.value(QStringLiteral("class")).toString(), kind = r.value(QStringLiteral("kind")).toString();
                    const int count = r.value(QStringLiteral("count")).toInt(-1);
                    if (!connectClasses().contains(cls) || !kindRe().match(kind).hasMatch() || count < 1 || count > 10000 ||
                        (r.contains(QStringLiteral("country")) && !countryRe().match(r.value(QStringLiteral("country")).toString()).hasMatch()))
                        return QStringLiteral("bad connect row");
                    const QString key = cls + u'|' + kind + u'|' + r.value(QStringLiteral("country")).toString();
                    if (keys.contains(key)) return QStringLiteral("duplicate connect row");
                    keys.insert(key);
                }
            }
            if (QJsonDocument(b).toJson(QJsonDocument::Compact).size() > kMaxBodyBytes) return QStringLiteral("too large");
            return {};
        }

        // The weekly install id: a new one when there is none or it is 7 days old (by UTC day).
        bool installDue(const QString &install, const QDate &since, const QDate &today) {
            return !uuidV4Re().match(install).hasMatch() || !since.isValid() || since > today || since.daysTo(today) >= 7;
        }

        // The wait after [fails] failures in a row this UTC day: 30, 60, 120, 240 min, then 6 h.
        qint64 backoffSecs(int fails) {
            if (fails <= 0) return 0;
            qint64 secs = kBackoffFirstSecs;
            for (int i = 1; i < fails && secs < kBackoffMaxSecs; ++i) secs *= 2;
            return qMin(secs, kBackoffMaxSecs);
        }

        // Whether to send a day's full summary again for its grown error counts: a past day (within
        // the contract's 14) once, today at most every 4 hours; never after a 400 refused errors today.
        bool refreshRule(const QString &day, const QString &today, int total, int sentErrors, const QString &refreshedDay,
                         const QString &errorsOffDay, qint64 sinceLastPost) {
            const QDate d = QDate::fromString(day, Qt::ISODate), t = QDate::fromString(today, Qt::ISODate);
            if (!d.isValid() || !t.isValid() || d > t || d.daysTo(t) > 14 || errorsOffDay == today) return false;
            if (total <= sentErrors) return false;
            if (day != today) return refreshedDay != day;
            return sinceLastPost >= 4 * 60 * 60;
        }

        // The delay before a day's first attempt: none for the very first summary of a fresh
        // install, else [roll] (0..1) of up to an hour, to spread the midnight-UTC spike.
        qint64 dayDelaySecs(bool freshInstall, double roll) {
            if (freshInstall) return 0;
            return qint64(qBound(0.0, roll, 1.0) * double(kDayDelayMaxSecs));
        }

        // ── state (UI thread) ────────────────────────────────────────────────

        struct Tele {
            QString file;          // <config dir>/telemetry.json
            QPointer<QTimer> timer;
            QPointer<QObject> owner;
            bool busy = false;
            int waits = 0;         // looks without a census verdict, this day
            QString waitsDay;
            // From the file:
            QString install;
            QDate installSince;
            QString sentDay;         // the UTC day whose summary is done
            qint64 holdUntil = 0;    // secs since epoch
            bool serverHold = false; // holdUntil came from a server 429/503
            int fails = 0;           // failures in a row on [failsDay]
            QString failsDay;
            QString startDay;        // the day [startAt] is for
            qint64 startAt = 0;      // a day's first attempt not before this (secs since epoch)
            QJsonObject pending;     // today's finished body, kept so a failed POST only POSTs again
            QJsonObject sentBody;    // the last body the server took (without errors), for an errors refresh
            int sentErrors = 0;      // the error total last sent for sentBody's day
            qint64 lastPostAt = 0;   // when sentBody last went
            QString errorsOffDay;    // a 400 came with errors this UTC day: no errors block until tomorrow
            QString refreshedDay;    // the past day already sent again for its errors (once only)
        };

        Tele &tele() {
            static Tele t;
            return t;
        }

        qint64 nowSecs() {
            return QDateTime::currentSecsSinceEpoch();
        }

        QDate utcToday() {
            return QDateTime::currentDateTimeUtc().date();
        }

        void load() {
            auto &t = tele();
            QFile f(t.file);
            if (!f.open(QIODevice::ReadOnly)) return;
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            t.install = o.value(QStringLiteral("install")).toString();
            t.installSince = QDate::fromString(o.value(QStringLiteral("install_since")).toString(), Qt::ISODate);
            t.sentDay = o.value(QStringLiteral("sent_day")).toString();
            t.holdUntil = o.value(QStringLiteral("hold_until")).toInteger();
            t.serverHold = o.value(QStringLiteral("server_hold")).toBool();
            t.fails = o.value(QStringLiteral("fails")).toInt();
            t.failsDay = o.value(QStringLiteral("fails_day")).toString();
            t.startDay = o.value(QStringLiteral("start_day")).toString();
            t.startAt = o.value(QStringLiteral("start_at")).toInteger();
            t.pending = o.value(QStringLiteral("pending")).toObject();
            t.sentBody = o.value(QStringLiteral("sent_body")).toObject();
            t.sentErrors = o.value(QStringLiteral("sent_errors")).toInt();
            t.lastPostAt = o.value(QStringLiteral("last_post_at")).toInteger();
            t.errorsOffDay = o.value(QStringLiteral("errors_off_day")).toString();
            t.refreshedDay = o.value(QStringLiteral("refreshed_day")).toString();
        }

        void save() {
            auto &t = tele();
            QSaveFile f(t.file);
            if (!f.open(QIODevice::WriteOnly)) return;
            QJsonObject o{{"install", t.install},
                          {"install_since", t.installSince.toString(Qt::ISODate)},
                          {"sent_day", t.sentDay},
                          {"hold_until", t.holdUntil},
                          {"server_hold", t.serverHold},
                          {"fails", t.fails},
                          {"fails_day", t.failsDay},
                          {"start_day", t.startDay},
                          {"start_at", t.startAt},
                          {"sent_errors", t.sentErrors},
                          {"last_post_at", t.lastPostAt},
                          {"errors_off_day", t.errorsOffDay},
                          {"refreshed_day", t.refreshedDay}};
            if (!t.pending.isEmpty()) o.insert(QStringLiteral("pending"), t.pending);
            if (!t.sentBody.isEmpty()) o.insert(QStringLiteral("sent_body"), t.sentBody);
            f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            f.commit();
        }

        QString currentInstall() {
            auto &t = tele();
            const QDate today = utcToday();
            if (installDue(t.install, t.installSince, today)) {
                QString id;
                do id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
                while (id == InstallId().toLower() || !uuidV4Re().match(id).hasMatch());
                t.install = id;
                t.installSince = today;
                save();
            }
            return t.install;
        }

        void look();

        enum class End {
            Sent,       // 200, or refused as it is (400/403/413, a body that breaks the contract): done today
            Failed,     // network, 5xx, 401, no site list, no token: exponential backoff
            ServerHold, // 429/503: the server's Retry-After
            Stale,      // the network or the tunnel changed during the pass: soon again
            Cancelled,  // the switch went off: nothing held
        };

        void finish(End end, qint64 serverSecs = 0) {
            auto &t = tele();
            t.busy = false;
            const QString today = utcToday().toString(Qt::ISODate);
            switch (end) {
                case End::Sent:
                    t.sentDay = today;
                    t.holdUntil = 0;
                    t.serverHold = false;
                    t.fails = 0;
                    t.pending = {};
                    break;
                case End::Failed:
                    if (t.failsDay != today) {
                        t.failsDay = today;
                        t.fails = 0;
                    }
                    ++t.fails;
                    t.holdUntil = nowSecs() + backoffSecs(t.fails);
                    t.serverHold = false;
                    break;
                case End::ServerHold:
                    t.holdUntil = nowSecs() + std::clamp<qint64>(serverSecs > 0 ? serverSecs : kServerHoldSecs, 60, 24 * 3600);
                    t.serverHold = true;
                    break;
                case End::Stale:
                    t.holdUntil = nowSecs() + kStaleHoldSecs;
                    t.serverHold = false;
                    break;
                case End::Cancelled:
                    return;
            }
            save();
        }

        enum class Purpose { Daily, Refresh };

        // [body] with the current errors block of its day, unless a 400 refused errors today
        // or the block would break the contract. [included]: the errors in the block; [dayTotal]:
        // the day's errors now, sent or not (what a later refresh compares against).
        QJsonObject withErrors(QJsonObject body, int *included, int *dayTotal) {
            body.remove(QStringLiteral("errors"));
            *included = 0;
            const QJsonObject e = errorsFor(body.value(QStringLiteral("day")).toString(), dayTotal);
            if (e.isEmpty() || tele().errorsOffDay == utcToday().toString(Qt::ISODate)) return body;
            QJsonObject with = body;
            with.insert(QStringLiteral("errors"), e);
            if (!contractProblem(with, InstallId().toLower(), utcToday()).isEmpty()) return body;
            *included = *dayTotal;
            return with;
        }

        // [body] is kept without errors; the block is added here, fresh. Daily: today's summary.
        // Refresh: the last sent body again, because its day's error counts grew since.
        void post(const QJsonObject &body, const QString &token, Purpose purpose) {
            int errorTotal = 0, dayTotal = 0;
            const QJsonObject sent = withErrors(body, &errorTotal, &dayTotal);
            const bool hasErrors = sent.contains(QStringLiteral("errors"));
            auto *nam = new QNetworkAccessManager(tele().owner);
            nam->setProxy(QNetworkProxy::NoProxy);
            QNetworkRequest request(QUrl(QLatin1String(ApiBase) + QLatin1String(kTelemetryPath)));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setRawHeader("Authorization", "Sl-Sub " + token.toUtf8());
            request.setRawHeader("X-Sl-Install-Id", InstallId().toUtf8());
            request.setRawHeader("X-Sl-Platform", "windows");
            request.setTransferTimeout(30 * 1000);
            const int rows = int(sent.value(QStringLiteral("listed")).toArray().size());
            auto *reply = nam->post(request, QJsonDocument(sent).toJson(QJsonDocument::Compact));
            QObject::connect(reply, &QNetworkReply::finished, nam, [reply, nam, rows, body, token, purpose, hasErrors, errorTotal, dayTotal] {
                reply->deleteLater();
                nam->deleteLater();
                auto &t = tele();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const qint64 retryAfter = reply->rawHeader("Retry-After").trimmed().toLongLong();
                const QString answer = status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString();
                LOG_INFO(QStringLiteral("[telemetry] %1 summary (%2 sites%3): %4")
                             .arg(purpose == Purpose::Daily ? "daily" : "errors refresh of the")
                             .arg(rows)
                             .arg(hasErrors ? QStringLiteral(", %1 errors").arg(errorTotal) : QString())
                             .arg(answer));
                Event("api", "call", status == 200 ? "ok" : "fail",
                      QStringLiteral("POST %1 %2").arg(QLatin1String(kTelemetryPath), status ? QString::number(status) : QStringLiteral("error")),
                      status == 200 ? QString() : answer);
                // Done with this body and the day's counts as they were, sent or left out: a
                // later refresh only follows new errors, and a past day goes again only once.
                const auto taken = [&t, &body, dayTotal, purpose] {
                    t.sentBody = body;
                    t.sentErrors = dayTotal;
                    t.lastPostAt = nowSecs();
                    const QString day = body.value(QStringLiteral("day")).toString();
                    if (purpose == Purpose::Refresh && day != utcToday().toString(Qt::ISODate)) t.refreshedDay = day;
                };
                if (status == 400 && hasErrors) {
                    // The server does not take the errors block (yet): once more today without
                    // it, so the site summary is never lost.
                    t.errorsOffDay = utcToday().toString(Qt::ISODate);
                    save();
                    if (purpose == Purpose::Daily) {
                        post(body, token, purpose);
                    } else {
                        // A refresh only carried errors: nothing more to send for these counts.
                        taken();
                        t.busy = false;
                        save();
                    }
                    return;
                }
                if (purpose == Purpose::Refresh) {
                    t.busy = false;
                    if (status == 200 || status == 400 || status == 403 || status == 413) taken(); // done with these counts
                    if (status == 429 || status == 503) finish(End::ServerHold, retryAfter);
                    else if (status != 200 && status != 400 && status != 403 && status != 413) finish(End::Failed);
                    else save();
                    return;
                }
                if (status == 200) {
                    taken();
                    finish(End::Sent);
                }
                else if (status == 429 || status == 503) finish(End::ServerHold, retryAfter);
                // Refused as it is (bad body, unknown app, too large): not again today.
                else if (status == 400 || status == 403 || status == 413) finish(End::Sent);
                else finish(End::Failed);
            });
        }

        // One probe pass over the site list, straight from the user's network, a few sites at a
        // time. The switch is checked before each probe: off stops the pass (in-flight probes
        // are aborted) and it ends as cancelled.
        class Pass : public QObject {
        public:
            using Done = std::function<void(const QList<QPair<QString, Outcome>> &, bool cancelled)>;

            Pass(QObject *parent, std::vector<Site> sites, Done done)
                : QObject(parent), sites_(std::move(sites)), done_(std::move(done)) {
                nam_ = new QNetworkAccessManager(this);
                nam_->setProxy(QNetworkProxy::NoProxy);
                results_.resize(int(sites_.size()));
                QTimer::singleShot(0, this, [this] { next(); });
            }

        private:
            // Starts probes up to the parallel limit; completes when none is left.
            void next() {
                if (finished_) return;
                if (!ConsentOn()) {
                    cancel();
                    return;
                }
                while (running_ < kParallelProbes && next_ < int(sites_.size())) start(next_++);
                if (running_ == 0 && next_ >= int(sites_.size())) complete(false);
            }

            void cancel() {
                finished_ = true;
                for (auto *reply : nam_->findChildren<QNetworkReply *>()) {
                    reply->disconnect(this);
                    reply->abort();
                }
                LOG_INFO(QStringLiteral("[telemetry] probe pass stopped: the switch was turned off"));
                complete(true);
            }

            void start(int i) {
                const Site &site = sites_[size_t(i)];
                QNetworkRequest request(site.url);
                request.setHeader(QNetworkRequest::UserAgentHeader, QLatin1String(kUserAgent));
                if (site.redirects > 0) {
                    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                    request.setMaximumRedirectsAllowed(site.redirects);
                } else {
                    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
                }
                if (!site.cookies) {
                    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
                    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
                }
                request.setTransferTimeout(site.timeoutMs);
                auto answer = std::make_shared<Answer>();
                auto done = std::make_shared<bool>(false);
                ++running_;
                auto *reply = nam_->get(request);
                const bool verify = site.verifyTls;
                const int maxBody = site.maxBody;
                QObject::connect(reply, &QNetworkReply::sslErrors, this, [reply, verify, answer](const QList<QSslError> &) {
                    if (verify) answer->tlsFailed = true;
                    else reply->ignoreSslErrors();
                });
                QObject::connect(reply, &QNetworkReply::redirected, this, [answer](const QUrl &url) { answer->redirects << url.toString(); });
                const auto capture = [reply, answer] {
                    answer->status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    answer->headers = reply->rawHeaderPairs();
                };
                const auto conclude = [this, i, reply, answer, done, capture](bool enough) {
                    if (*done || finished_) return;
                    *done = true;
                    capture();
                    if (!enough) answer->error = reply->error();
                    results_[i] = {sites_[size_t(i)].id, classify(sites_[size_t(i)], *answer)};
                    reply->disconnect(this);
                    reply->abort();
                    reply->deleteLater();
                    --running_;
                    // The next probe from the event loop, never inside this reply's own signal.
                    QTimer::singleShot(0, this, [this] { next(); });
                };
                QObject::connect(reply, &QNetworkReply::readyRead, this, [reply, answer, maxBody, conclude] {
                    answer->body += reply->read(qMax<qint64>(0, maxBody - answer->body.size()));
                    // Enough of the page to classify it: the rest is not fetched.
                    if (answer->body.size() >= maxBody) conclude(true);
                });
                QObject::connect(reply, &QNetworkReply::finished, this, [reply, answer, maxBody, conclude] {
                    // A failed reply is closed already: nothing to read.
                    if (reply->isOpen()) answer->body += reply->read(qMax<qint64>(0, maxBody - answer->body.size()));
                    conclude(false);
                });
                // The whole chain gets three times a hop's budget.
                QTimer::singleShot(site.timeoutMs * 3, this, [answer, done, conclude] {
                    if (*done) return;
                    answer->timedOut = true;
                    conclude(false);
                });
            }

            void complete(bool cancelled) {
                finished_ = true;
                QList<QPair<QString, Outcome>> rows;
                if (!cancelled) {
                    for (const auto &r : results_) {
                        if (!r.first.isEmpty()) rows << r;
                    }
                }
                const auto done = std::move(done_);
                deleteLater();
                done(rows, cancelled);
            }

            std::vector<Site> sites_;
            Done done_;
            QNetworkAccessManager *nam_ = nullptr;
            QList<QPair<QString, Outcome>> results_;
            int next_ = 0;
            int running_ = 0;
            bool finished_ = false;
        };

        bool staleNow(const QByteArray &net, int epoch) {
            QString type;
            return FreshNetworkKey(&type) != net || (ConnectCount() != epoch && settings()->spmode_vpn) || !OffVpnNow();
        }

        // Whether the last sent body should go again for its day's grown error counts: a past
        // day (within the contract's 14) at once, today at most every 4 hours.
        bool refreshDue(const QString &today) {
            auto &t = tele();
            if (t.sentBody.isEmpty()) return false;
            const QString day = t.sentBody.value(QStringLiteral("day")).toString();
            int total = 0;
            errorsFor(day, &total);
            return refreshRule(day, today, total, t.sentErrors, t.refreshedDay, t.errorsOffDay, nowSecs() - t.lastPostAt);
        }

        // A new UTC day: the backoff starts over (a server hold stands), yesterday's unsent body
        // goes, and the day's first attempt gets its random delay.
        void rollDay(const QString &today) {
            auto &t = tele();
            bool changed = false;
            if (t.failsDay != today) {
                t.failsDay = today;
                t.fails = 0;
                if (!t.serverHold) t.holdUntil = 0;
                changed = true;
            }
            if (!t.pending.isEmpty() && t.pending.value(QStringLiteral("day")).toString() != today) {
                t.pending = {};
                changed = true;
            }
            if (t.startDay != today) {
                const bool fresh = t.sentDay.isEmpty() && t.install.isEmpty();
                t.startDay = today;
                t.startAt = nowSecs() + dayDelaySecs(fresh, QRandomGenerator::global()->generateDouble());
                changed = true;
            }
            if (changed) save();
        }

        // Every 10 minutes: when today's summary is still due and the switch is on, the site
        // list, one probe pass and one upload (or only the upload, when today's pass is kept).
        void look() {
            auto &t = tele();
            const QString today = utcToday().toString(Qt::ISODate);
            if (t.busy || !ConsentOn()) return;
            rollDay(today);
            if (nowSecs() < t.holdUntil) return;
            const bool refresh = refreshDue(today);
            if (!refresh && (t.sentDay == today || nowSecs() < t.startAt)) return;
            QString token;
            if (!ApiToken(&token)) return;
            QString netType;
            bool checked = false;
            const QByteArray net = CurrentNetworkKey(&netType, &checked);
            // The upload goes on the direct road too: the server reads region and provider from it.
            if (!checked || net.isEmpty() || !OffVpnNow()) return;

            // The last sent day's error counts grew: the same body again with them (the server
            // keeps the last upload per install and day, so the site rows stay).
            if (refresh) {
                t.busy = true;
                post(t.sentBody, token, Purpose::Refresh);
                return;
            }

            // Today's pass is done and only its upload failed: send the same body again.
            if (!t.pending.isEmpty()) {
                t.busy = true;
                post(t.pending, token, Purpose::Daily);
                return;
            }

            // The census's verdict for this network; after a few looks without one, «unknown».
            if (t.waitsDay != today) {
                t.waitsDay = today;
                t.waits = 0;
            }
            bool measured = false;
            const QString blocking = CensusBlockingWord(&measured);
            if (!measured && t.waits++ < kWaitCensusLooks) return;

            t.busy = true;
            const int epoch = ConnectCount();
            auto *nam = new QNetworkAccessManager(t.owner);
            nam->setProxy(QNetworkProxy::NoProxy);
            QNetworkRequest request(QUrl(QLatin1String(ApiBase) + QLatin1String(kProbesPath)));
            request.setTransferTimeout(15 * 1000);
            auto *reply = nam->get(request);
            QObject::connect(reply, &QNetworkReply::finished, nam, [reply, nam, net, netType, blocking, epoch] {
                reply->deleteLater();
                nam->deleteLater();
                // Turned off while the list was on its way: no probe at all.
                if (!ConsentOn()) {
                    finish(End::Cancelled);
                    return;
                }
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
                if (status != 200 || doc.isEmpty()) {
                    LOG_INFO(QStringLiteral("[telemetry] site list: %1").arg(status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString()));
                    finish(End::Failed);
                    return;
                }
                new Pass(tele().owner, parseSites(doc), [net, netType, blocking, epoch](const QList<QPair<QString, Outcome>> &rows, bool cancelled) {
                    if (cancelled || !ConsentOn()) {
                        finish(End::Cancelled);
                        return;
                    }
                    if (staleNow(net, epoch)) {
                        // Another network, or a tunnel that may have carried the probes.
                        LOG_INFO(QStringLiteral("[telemetry] probe pass dropped: the network or the tunnel changed"));
                        finish(End::Stale);
                        return;
                    }
                    QString token;
                    if (!ApiToken(&token)) {
                        finish(End::Failed);
                        return;
                    }
                    const QJsonObject body = buildBody(currentInstall(), utcToday().toString(Qt::ISODate), netType, blocking, rows);
                    if (const QString problem = contractProblem(body, InstallId().toLower(), utcToday()); !problem.isEmpty()) {
                        LOG_INFO(QStringLiteral("[telemetry] summary not sent: %1").arg(problem));
                        finish(End::Sent);
                        return;
                    }
                    // Kept until the server takes it, so a failed POST does not probe again.
                    tele().pending = body;
                    save();
                    post(body, token, Purpose::Daily);
                });
            });
        }

        // The switch was turned on: look again soon. A server's Retry-After still stands.
        void scheduleSoon(int ms) {
            auto &t = tele();
            if (!t.timer) return;
            if (!t.serverHold) t.holdUntil = 0;
            QTimer::singleShot(ms, t.timer, [] { look(); });
        }

    } // namespace

    void InstallTelemetry(QObject *owner) {
        auto &t = tele();
        t.owner = owner;
        t.file = QDir::current().absoluteFilePath(QStringLiteral("telemetry.json"));
        load();
        t.timer = new QTimer(owner);
        t.timer->setInterval(kLookEveryMs);
        QObject::connect(t.timer, &QTimer::timeout, owner, [] { look(); });
        t.timer->start();
        QTimer::singleShot(kFirstLookMs, t.timer, [] { look(); });
    }

    QString TelemetrySelfTest() {
        QStringList bad;
        const auto expect = [&bad](const char *name, bool ok) {
            if (!ok) bad << QString::fromLatin1(name);
        };

        // The site list: defaults under each probe, bad ids and non-https URLs left out.
        const QJsonObject doc = QJsonDocument::fromJson(QByteArray(R"({
            "probe_defaults": {"timeout_ms": 4000, "follow_redirects": 5, "verify_tls": true, "max_body_bytes": 16384},
            "sites": [
              {"id": "ozon", "probe": {"url": "https://www.ozon.ru/", "cookies": true, "follow_redirects": 2}, "ok_status": [200],
               "markers": [{"class": "captcha", "status": 403, "title_contains": "Antibot"}, {"class": "captcha", "redirect_contains": "__rr="}]},
              {"id": "sber", "probe": {"url": "https://www.sberbank.ru/", "verify_tls": false}, "ok_status": [200, 301, 302]},
              {"id": "chatgpt", "probe": {"url": "https://chatgpt.com/"}, "ok_status": [200],
               "markers": [{"class": "captcha", "status": 403, "header": {"name": "cf-mitigated", "contains": "challenge"}}]},
              {"id": "Bad Id", "probe": {"url": "https://x.example/"}},
              {"id": "plain", "probe": {"url": "http://x.example/"}},
              {"id": "ozon", "probe": {"url": "https://again.example/"}}
            ]})")).object();
        const auto sites = parseSites(doc);
        expect("parse: three sites", sites.size() == 3);
        if (sites.size() != 3) return bad.join(QStringLiteral("; "));
        expect("parse: own redirects", sites[0].redirects == 2 && sites[0].cookies && sites[0].timeoutMs == 4000);
        expect("parse: verify_tls false", !sites[1].verifyTls && sites[1].okStatus.size() == 3);

        const auto outcome = [&](const Site &s, const Answer &a, bool works, const char *fail) {
            const Outcome o = classify(s, a);
            return o.works == works && o.fail == QLatin1String(fail);
        };
        Answer ok200;
        ok200.status = 200;
        ok200.body = "<html><title>Ozon</title></html>";
        expect("200 opens", outcome(sites[0], ok200, true, ""));
        Answer antibot;
        antibot.status = 403;
        antibot.body = "<title>Antibot Challenge Page</title>";
        expect("antibot page is captcha", outcome(sites[0], antibot, false, "captcha"));
        Answer redirect = ok200;
        redirect.redirects << QStringLiteral("https://www.ozon.ru/?__rr=1");
        expect("antibot redirect is captcha", outcome(sites[0], redirect, false, "captcha"));
        Answer plain403;
        plain403.status = 403;
        expect("other 403 is http403", outcome(sites[0], plain403, false, "http403"));
        Answer cf;
        cf.status = 403;
        cf.headers << QNetworkReply::RawHeaderPair{"CF-Mitigated", "challenge"};
        expect("header marker", outcome(sites[2], cf, false, "captcha"));
        Answer moved;
        moved.status = 302;
        expect("ok_status 302", outcome(sites[1], moved, true, ""));
        Answer s451;
        s451.status = 451;
        expect("451", outcome(sites[1], s451, false, "http451"));
        Answer gone;
        gone.status = 500;
        expect("500: no key", outcome(sites[1], gone, false, ""));
        Answer slow;
        slow.timedOut = true;
        expect("timeout", outcome(sites[1], slow, false, "timeout"));
        Answer refused;
        refused.error = QNetworkReply::ConnectionRefusedError;
        expect("refused", outcome(sites[1], refused, false, "refused"));
        Answer reset;
        reset.error = QNetworkReply::RemoteHostClosedError;
        expect("reset", outcome(sites[1], reset, false, "reset"));
        Answer tls;
        tls.tlsFailed = true;
        expect("tls", outcome(sites[2], tls, false, "tls"));
        Answer dns;
        dns.error = QNetworkReply::HostNotFoundError;
        expect("name failure: no key", outcome(sites[2], dns, false, ""));

        // The body against the contract.
        const QDate today(2026, 10, 28);
        const QString install = QStringLiteral("9a1b2c3d-4e5f-4a6b-9c7d-8e9f0a1b2c3d");
        const QString auth = QStringLiteral("0a1b2c3d-4e5f-4a6b-9c7d-8e9f0a1b2c3d");
        QList<QPair<QString, Outcome>> rows{{QStringLiteral("ozon"), {false, QStringLiteral("captcha")}},
                                            {QStringLiteral("sber"), {true, {}}}};
        const auto rows0 = [&rows] { return rows; };
        const QJsonObject body = buildBody(install, today.toString(Qt::ISODate), QStringLiteral("wifi"), QStringLiteral("regular"), rows);
        const QString problem = contractProblem(body, auth, today);
        if (!problem.isEmpty()) bad << QStringLiteral("good body refused: ") + problem;
        expect("no region, ip or host in the body", !body.contains(QStringLiteral("geo")) && !QJsonDocument(body).toJson().contains("sberbank"));
        expect("auth install id refused", !contractProblem(body, install, today).isEmpty());
        expect("old day refused", !contractProblem(buildBody(install, QStringLiteral("2026-10-13"), "wifi", "none", rows), auth, today).isEmpty());
        expect("future day refused", !contractProblem(buildBody(install, QStringLiteral("2026-10-29"), "wifi", "none", rows), auth, today).isEmpty());
        expect("bad blocking refused", !contractProblem(buildBody(install, QStringLiteral("2026-10-28"), "wifi", "maybe", rows), auth, today).isEmpty());
        expect("none net type is other", buildBody(install, QStringLiteral("2026-10-28"), "none", "unknown", {}).value("net").toObject().value("type") == "other");
        QJsonObject extra = body;
        extra.insert(QStringLiteral("city"), QStringLiteral("x"));
        expect("unknown key refused", !contractProblem(extra, auth, today).isEmpty());

        // The errors block: classes, labels, limits, omission.
        expect("class: no answer is a timeout", connectClass(false, {}) == "timeout");
        expect("class: dns", connectClass(true, "dial tcp: lookup x.example: no such host") == "dns");
        expect("class: tls", connectClass(true, "x509: certificate signed by unknown authority") == "tls");
        expect("class: handshake", connectClass(true, "handshake failed: EOF") == "handshake");
        expect("class: tcp", connectClass(true, "dial tcp 203.0.113.7:443: connectex: connection refused") == "tcp");
        expect("class: timeout", connectClass(true, "context deadline exceeded") == "timeout");
        expect("class: other", connectClass(true, "start service: initialize rule-set") == "other");
        expect("kind vless reality", kindLabel("xrayvless", "", true) == "vless_reality");
        expect("kind vless xhttp", kindLabel("vless", "XHTTP", true) == "vless_xhttp");
        expect("kind trojan ws", kindLabel("trojan", "WebSocket", false) == "trojan_ws");
        expect("kind ss", kindLabel("shadowsocks", "", false) == "ss");
        expect("kind hysteria2", kindLabel("hysteria2", "", false) == "hysteria2");
        expect("kind never a name", kindLabel("", "", false) == "other" && kindRe().match(kindLabel("A Very-Long Custom Type Name!!", "gRPC", false)).hasMatch());
        {
            DayErrors none;
            none.connect.insert("tls|vless_reality|NL", 0);
            expect("all zero: no block", errorsBlock(none).isEmpty());
            DayErrors d;
            d.crashes = 1;
            d.connect.insert("tls|vless_reality|NL", 7);
            d.connect.insert("no_rx|hysteria2|", 3);
            d.connect.insert("tcp|ss|", 0);
            const QJsonObject e = errorsBlock(d);
            const QJsonArray rows = e.value("connect").toArray();
            expect("block: crashes, no zero abnormal, two rows", e.value("crashes").toInt() == 1 && !e.contains("abnormal_exits") && rows.size() == 2);
            expect("block: most frequent first, country omitted when unknown",
                   rows.size() == 2 && rows[0].toObject().value("count").toInt() == 7 && rows[0].toObject().value("country") == "NL" &&
                       !rows[1].toObject().contains("country"));
            DayErrors many;
            for (int i = 0; i < 150; ++i) many.connect.insert(QStringLiteral("other|k%1|").arg(i), i + 1);
            many.abnormal = 20000;
            const QJsonObject m = errorsBlock(many);
            expect("block: at most 100 rows, counts capped", m.value("connect").toArray().size() == 100 &&
                                                                m.value("connect").toArray()[0].toObject().value("count").toInt() == 150 &&
                                                                m.value("abnormal_exits").toInt() == 10000);
            QJsonObject withE = buildBody(install, today.toString(Qt::ISODate), "wifi", "regular", rows0());
            withE.insert("errors", e);
            const QString pe = contractProblem(withE, auth, today);
            if (!pe.isEmpty()) bad << QStringLiteral("errors body refused: ") + pe;
            QJsonObject badRow = withE;
            QJsonObject eb = e;
            eb.insert("connect", QJsonArray{QJsonObject{{"class", "tls"}, {"kind", "vless"}, {"count", 0}}});
            badRow.insert("errors", eb);
            expect("zero count refused", !contractProblem(badRow, auth, today).isEmpty());
            eb.insert("connect", QJsonArray{QJsonObject{{"class", "tls"}, {"kind", "vless"}, {"count", 1}, {"host", "x"}}});
            badRow.insert("errors", eb);
            expect("host in a row refused", !contractProblem(badRow, auth, today).isEmpty());
            badRow.insert("errors", QJsonObject{});
            expect("empty errors block refused", !contractProblem(badRow, auth, today).isEmpty());
        }

        // The errors refresh: a past day once, today every 4 h, no loop when the block was left out.
        {
            const QString d28 = "2026-10-28", d29 = "2026-10-29";
            expect("refresh: a past day's grown counts go once", refreshRule(d28, d29, 3, 1, {}, {}, 60));
            expect("refresh: a past day only once", !refreshRule(d28, d29, 5, 3, d28, {}, 60));
            expect("refresh: today not within 4 h", !refreshRule(d29, d29, 3, 1, {}, {}, 3600));
            expect("refresh: today after 4 h", refreshRule(d29, d29, 3, 1, {}, {}, 4 * 3600));
            expect("refresh: no growth, no send", !refreshRule(d28, d29, 2, 2, {}, {}, 60));
            // A send that left the block out records the day's total: nothing grows afterwards.
            expect("refresh: a left-out block does not loop", !refreshRule(d28, d29, 2, 2, {}, {}, 60));
            expect("refresh: not after a 400 refused errors today", !refreshRule(d28, d29, 3, 1, {}, d29, 60));
            expect("refresh: not past the 14 days", !refreshRule("2026-10-10", d29, 3, 1, {}, {}, 60));
        }

        // The backoff and the day's delay.
        expect("backoff 30/60/120/240 min, then 6 h",
               backoffSecs(0) == 0 && backoffSecs(1) == 1800 && backoffSecs(2) == 3600 && backoffSecs(3) == 7200 &&
                   backoffSecs(4) == 14400 && backoffSecs(5) == 21600 && backoffSecs(40) == 21600);
        expect("no day delay for a fresh install's first summary", dayDelaySecs(true, 0.9) == 0);
        expect("day delay within an hour", dayDelaySecs(false, 0.0) == 0 && dayDelaySecs(false, 0.5) == 1800 &&
                                               dayDelaySecs(false, 1.0) == 3600 && dayDelaySecs(false, 7.0) == 3600);

        // The weekly id.
        expect("no id: new one", installDue({}, {}, today));
        expect("6 days: kept", !installDue(install, today.addDays(-6), today));
        expect("7 days: rotated", installDue(install, today.addDays(-7), today));
        expect("clock set back: rotated", installDue(install, today.addDays(1), today));
        return bad.join(QStringLiteral("; "));
    }

} // namespace Shadowlos::Journal
