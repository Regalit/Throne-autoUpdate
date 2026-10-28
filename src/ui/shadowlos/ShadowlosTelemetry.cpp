#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "include/global/ShadowlosBootstrap.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
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

    bool ConsentOn() {
        return settings()->shadowlos_send_journal != 0;
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
#if SHADOWLOS_DEV
        JournalConnectEnd(ok);
#endif
    }

    void Disconnect(bool crash, bool manual) {
        {
            QMutexLocker lock(&conn().mutex);
            conn().up = false;
        }
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
            settings()->Save();
#if SHADOWLOS_DEV
            JournalConsentChanged(on);
#endif
            LOG_INFO(QStringLiteral("[telemetry] «Улучшение обхода» turned %1").arg(on ? "on" : "off"));
            if (on) scheduleSoon(30 * 1000);
        });
        consentAction = action;
#if SHADOWLOS_DEV
        InstallJournal(window, tools, action, tun, systemProxy);
#else
        Q_UNUSED(tools)
        Q_UNUSED(tun)
        Q_UNUSED(systemProxy)
#endif
        InstallGeo(window);
        InstallTelemetry(window);
    }

    // ── the daily summary ────────────────────────────────────────────────────

    namespace {

        constexpr auto kTelemetryPath = "/api/mobile/telemetry";
        constexpr auto kProbesPath = "/api/mobile/telemetry/probes.json";
        constexpr auto kAppId = SHADOWLOS_DEV ? "shadowlos-throne-dev" : "shadowlos-throne";
        constexpr int kFirstLookMs = 90 * 1000;
        constexpr int kLookEveryMs = 10 * 60 * 1000;
        constexpr qint64 kRetrySecs = 30 * 60;
        constexpr qint64 kHoldSecs = 60 * 60;
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
                                           "kind", "net", "listed", "unlisted", "probe"};
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
            if (QJsonDocument(b).toJson(QJsonDocument::Compact).size() > kMaxBodyBytes) return QStringLiteral("too large");
            return {};
        }

        // The weekly install id: a new one when there is none or it is 7 days old (by UTC day).
        bool installDue(const QString &install, const QDate &since, const QDate &today) {
            return !uuidV4Re().match(install).hasMatch() || !since.isValid() || since > today || since.daysTo(today) >= 7;
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
            QString sentDay;
            qint64 holdUntil = 0;  // secs since epoch
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
        }

        void save() {
            auto &t = tele();
            QSaveFile f(t.file);
            if (!f.open(QIODevice::WriteOnly)) return;
            f.write(QJsonDocument(QJsonObject{{"install", t.install},
                                              {"install_since", t.installSince.toString(Qt::ISODate)},
                                              {"sent_day", t.sentDay},
                                              {"hold_until", t.holdUntil}})
                        .toJson(QJsonDocument::Compact));
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

        void hold(qint64 secs) {
            tele().holdUntil = nowSecs() + std::clamp<qint64>(secs, 60, 24 * 3600);
            save();
        }

        void look();

        void finish(bool ok, qint64 retrySecs = kRetrySecs) {
            auto &t = tele();
            t.busy = false;
            if (ok) {
                t.sentDay = utcToday().toString(Qt::ISODate);
                t.holdUntil = 0;
                save();
            } else {
                hold(retrySecs);
            }
        }

        void post(const QJsonObject &body, const QString &token) {
            auto *nam = new QNetworkAccessManager(tele().owner);
            nam->setProxy(QNetworkProxy::NoProxy);
            QNetworkRequest request(QUrl(QLatin1String(ApiBase) + QLatin1String(kTelemetryPath)));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setRawHeader("Authorization", "Sl-Sub " + token.toUtf8());
            request.setRawHeader("X-Sl-Install-Id", InstallId().toUtf8());
            request.setRawHeader("X-Sl-Platform", "windows");
            request.setTransferTimeout(30 * 1000);
            const int rows = int(body.value(QStringLiteral("listed")).toArray().size());
            auto *reply = nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
            QObject::connect(reply, &QNetworkReply::finished, nam, [reply, nam, rows] {
                reply->deleteLater();
                nam->deleteLater();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const qint64 retryAfter = reply->rawHeader("Retry-After").trimmed().toLongLong();
                const QString answer = status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString();
                LOG_INFO(QStringLiteral("[telemetry] daily summary (%1 sites): %2").arg(rows).arg(answer));
                Event("api", "call", status == 200 ? "ok" : "fail",
                      QStringLiteral("POST %1 %2").arg(QLatin1String(kTelemetryPath), status ? QString::number(status) : QStringLiteral("error")),
                      status == 200 ? QString() : answer);
                if (status == 200) finish(true);
                else if (status == 429 || status == 503) finish(false, retryAfter > 0 ? retryAfter : kHoldSecs);
                // Refused as it is (bad body, unknown app, too large): not again today.
                else if (status == 400 || status == 403 || status == 413) finish(true);
                else if (status == 401) finish(false, kHoldSecs);
                else finish(false);
            });
        }

        // One probe pass over the site list, straight from the user's network.
        class Pass : public QObject {
        public:
            using Done = std::function<void(const QList<QPair<QString, Outcome>> &)>;

            Pass(QObject *parent, std::vector<Site> sites, Done done)
                : QObject(parent), sites_(std::move(sites)), done_(std::move(done)) {
                nam_ = new QNetworkAccessManager(this);
                nam_->setProxy(QNetworkProxy::NoProxy);
                results_.resize(int(sites_.size()));
                pending_ = int(sites_.size());
                if (pending_ == 0) {
                    QTimer::singleShot(0, this, [this] { complete(); });
                    return;
                }
                for (int i = 0; i < int(sites_.size()); ++i) start(i);
            }

        private:
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
                    if (*done) return;
                    *done = true;
                    capture();
                    if (!enough) answer->error = reply->error();
                    results_[i] = {sites_[size_t(i)].id, classify(sites_[size_t(i)], *answer)};
                    reply->disconnect(this);
                    reply->abort();
                    reply->deleteLater();
                    if (--pending_ == 0) complete();
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

            void complete() {
                QList<QPair<QString, Outcome>> rows;
                for (const auto &r : results_) {
                    if (!r.first.isEmpty()) rows << r;
                }
                const auto done = std::move(done_);
                deleteLater();
                done(rows);
            }

            std::vector<Site> sites_;
            Done done_;
            QNetworkAccessManager *nam_ = nullptr;
            QList<QPair<QString, Outcome>> results_;
            int pending_ = 0;
        };

        bool staleNow(const QByteArray &net, int epoch) {
            QString type;
            return FreshNetworkKey(&type) != net || (ConnectCount() != epoch && settings()->spmode_vpn) || !OffVpnNow();
        }

        // Every 10 minutes: when today's summary is still due and the switch is on, the site
        // list, one probe pass and one upload.
        void look() {
            auto &t = tele();
            const QString today = utcToday().toString(Qt::ISODate);
            if (t.busy || !ConsentOn() || t.sentDay == today || nowSecs() < t.holdUntil) return;
            QString token;
            if (!ApiToken(&token)) return;
            QString netType;
            bool checked = false;
            const QByteArray net = CurrentNetworkKey(&netType, &checked);
            if (!checked || net.isEmpty() || !OffVpnNow()) return;
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
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
                if (status != 200 || doc.isEmpty()) {
                    LOG_INFO(QStringLiteral("[telemetry] site list: %1").arg(status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString()));
                    finish(false);
                    return;
                }
                new Pass(tele().owner, parseSites(doc), [net, netType, blocking, epoch](const QList<QPair<QString, Outcome>> &rows) {
                    if (!ConsentOn()) {
                        tele().busy = false;
                        return;
                    }
                    if (staleNow(net, epoch)) {
                        // Another network, or a tunnel that may have carried the probes.
                        LOG_INFO(QStringLiteral("[telemetry] probe pass dropped: the network or the tunnel changed"));
                        finish(false, 5 * 60);
                        return;
                    }
                    QString token;
                    if (!ApiToken(&token)) {
                        finish(false);
                        return;
                    }
                    const QJsonObject body = buildBody(currentInstall(), utcToday().toString(Qt::ISODate), netType, blocking, rows);
                    if (const QString problem = contractProblem(body, InstallId().toLower(), utcToday()); !problem.isEmpty()) {
                        LOG_INFO(QStringLiteral("[telemetry] summary not sent: %1").arg(problem));
                        finish(true);
                        return;
                    }
                    post(body, token);
                });
            });
        }

        void scheduleSoon(int ms) {
            auto &t = tele();
            if (!t.timer) return;
            t.holdUntil = 0;
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

        // The weekly id.
        expect("no id: new one", installDue({}, {}, today));
        expect("6 days: kept", !installDue(install, today.addDays(-6), today));
        expect("7 days: rotated", installDue(install, today.addDays(-7), today));
        expect("clock set back: rotated", installDue(install, today.addDays(1), today));
        return bad.join(QStringLiteral("; "));
    }

} // namespace Shadowlos::Journal
