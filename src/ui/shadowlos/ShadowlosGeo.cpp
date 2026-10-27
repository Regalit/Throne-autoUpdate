#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif

#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#if SHADOWLOS_DEV

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "include/global/ShadowlosBootstrap.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <climits>

// Where the user is and on what kind of network (journal-format.md, "Addendum: net step and
// geo"): net_type from the active adapter, country/region/asn/org from our
// GET /api/mobile/whereami made on the user's own network. carrier and mcc_mnc do not exist
// on Windows, and no city is kept anywhere (shadowlos 2feb9c58: it narrows a person down).
// No IP is ever kept: the server's answer has none, and every value goes through SanitizeGeo
// before it is used.
namespace Shadowlos::Journal {

    namespace {

        constexpr int kCheckEveryMs = 15 * 1000;
        constexpr int kStartLineMs = 6 * 1000;
        constexpr qint64 kFreshSecs = 30 * 60;
        constexpr qint64 kRetrySecs = 5 * 60;
        constexpr auto kWhereamiPath = "/api/mobile/whereami";
        // The server refuses whereami without it (handlers/whereami.go whereamiApps).
        constexpr auto kAppHeader = "shadowlos-throne-dev";

        // ── sanitizing ───────────────────────────────────────────────────────

        // Free text the server accepts (client_logs.go clientLogGeo.valid): short, no control
        // characters and nothing a scrub would redact. Stricter than the server on purpose:
        // one bad field makes it refuse the whole upload with 400.
        bool plainText(const QString &s, int maxBytes) {
            if (s.isEmpty() || s.trimmed() != s || s.toUtf8().size() > maxBytes) return false;
            for (const QChar c : s) {
                if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
            }
            // A dotted name (amazon.com, x.example.de), whatever its case; "St. Petersburg" stays.
            static const QRegularExpression dotted(QStringLiteral(R"([A-Za-z0-9-]\.[A-Za-z0-9])"));
            // The server's key=value rule, which has the [_-] variants ours lacks.
            static const QRegularExpression keyValue(QStringLiteral(
                R"(\b(password|passwd|pwd|pass|token|secret|psk|private[_-]?key|public[_-]?key|key|auth|authorization|cookie|uuid|sid|pbk|sni|host|server|address|addr|telegram[_-]?id|tg[_-]?id|tgid|user[_-]?id|userid)\s*[=:])"),
                QRegularExpression::CaseInsensitiveOption);
            if (dotted.match(s).hasMatch() || keyValue.match(s).hasMatch()) return false;
            return Scrub(s) == s;
        }

        QString shaped(const QJsonValue &v, const QRegularExpression &re) {
            const QString s = v.toString();
            return re.match(s).hasMatch() ? s : QString();
        }

        // ── state (UI thread, except the latest object) ──────────────────────

        struct Net {
            QString type = QStringLiteral("none"); // ethernet, wifi, other, none
            QByteArray key;                         // sha256 hex of the network's identity; empty for none
        };

        struct Entry {
            QJsonObject geo;   // country region asn org, sanitized
            qint64 nextAt = 0; // when to ask again
        };

        struct Geo {
            QPointer<QNetworkAccessManager> nam;
            QHash<QByteArray, Entry> cache; // by network key; memory only
            QHash<QByteArray, QByteArray> gatewayMac; // last MAC seen per adapter+gateway, memory only
            Net net;
            bool checked = false;
            bool busy = false;
            QString pendingLine; // a net line waiting for the answer in flight ("" none)
            int connectEpoch = 0;
        };

        Geo &g() {
            static Geo geo;
            return geo;
        }

        QMutex &latestMutex() {
            static QMutex m;
            return m;
        }

        QJsonObject &latest() {
            static QJsonObject obj;
            return obj;
        }

        Configs::SettingsRepo *settings() {
            return Configs::dataManager->settingsRepo.get();
        }

        // ── the active network ────────────────────────────────────────────────

#ifdef Q_OS_WIN
        // The address bytes (hashed later, never shown).
        QByteArray sockaddrBytes(const SOCKET_ADDRESS &a) {
            if (a.lpSockaddr->sa_family == AF_INET) {
                const auto &in = reinterpret_cast<const sockaddr_in *>(a.lpSockaddr)->sin_addr;
                return QByteArray(reinterpret_cast<const char *>(&in), sizeof in);
            }
            if (a.lpSockaddr->sa_family == AF_INET6) {
                const auto &in = reinterpret_cast<const sockaddr_in6 *>(a.lpSockaddr)->sin6_addr;
                return QByteArray(reinterpret_cast<const char *>(&in), sizeof in);
            }
            return {};
        }

        // The gateway's MAC from the neighbour cache (never resolved here, so never blocks).
        QByteArray neighbourMac(const SOCKET_ADDRESS &a, NET_IFINDEX ifIndex) {
            MIB_IPNET_ROW2 row{};
            if (a.lpSockaddr->sa_family == AF_INET) row.Address.Ipv4 = *reinterpret_cast<const sockaddr_in *>(a.lpSockaddr);
            else if (a.lpSockaddr->sa_family == AF_INET6) row.Address.Ipv6 = *reinterpret_cast<const sockaddr_in6 *>(a.lpSockaddr);
            else return {};
            row.InterfaceIndex = ifIndex;
            if (GetIpNetEntry2(&row) != NO_ERROR || row.PhysicalAddressLength == 0) return {};
            return QByteArray(reinterpret_cast<const char *>(row.PhysicalAddress), int(row.PhysicalAddressLength));
        }

        // The up adapter with a gateway and the lowest metric, leaving out loopback, tunnels and
        // virtual adapters (our own Wintun TUN is one), so the answer is the same with the VPN on.
        Net currentNet() {
            Net net;
            QByteArray buf;
            ULONG size = 32 * 1024;
            ULONG ret = ERROR_BUFFER_OVERFLOW;
            for (int i = 0; i < 3 && ret == ERROR_BUFFER_OVERFLOW; ++i) {
                buf.resize(int(size));
                ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                           nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
            }
            if (ret != NO_ERROR) return net;
            const IP_ADAPTER_ADDRESSES *best = nullptr;
            ULONG bestMetric = ULONG_MAX;
            for (auto *a = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(buf.constData()); a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp || a->FirstGatewayAddress == nullptr) continue;
                if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PROP_VIRTUAL) continue;
                const QString desc = QString::fromWCharArray(a->Description).toLower();
                if (desc.contains(QStringLiteral("wintun")) || desc.contains(QStringLiteral("tap-windows"))) continue;
                const ULONG metric = a->Ipv4Enabled ? a->Ipv4Metric : a->Ipv6Metric;
                if (metric < bestMetric) {
                    bestMetric = metric;
                    best = a;
                }
            }
            if (best == nullptr) return net;
            net.type = best->IfType == IF_TYPE_IEEE80211 ? QStringLiteral("wifi")
                       : best->IfType == IF_TYPE_ETHERNET_CSMACD ? QStringLiteral("ethernet")
                                                                  : QStringLiteral("other");
            // Identity: adapter, gateway addresses, DNS suffix and the gateway's MAC (two home
            // networks both on 192.168.1.1 differ there). Only its hash is ever kept.
            QByteArray base = QByteArray(best->AdapterName) + '|' + QByteArray::number(int(best->IfType)) + '|' +
                              QString::fromWCharArray(best->DnsSuffix ? best->DnsSuffix : L"").toUtf8();
            QByteArray mac;
            for (auto *gw = best->FirstGatewayAddress; gw; gw = gw->Next) {
                base += '|' + sockaddrBytes(gw->Address);
                if (mac.isEmpty()) mac = neighbourMac(gw->Address, best->IfIndex);
            }
            const QByteArray baseHash = QCryptographicHash::hash(base, QCryptographicHash::Sha256);
            // A neighbour entry that is briefly missing must not look like a new network.
            if (mac.isEmpty()) mac = g().gatewayMac.value(baseHash);
            else g().gatewayMac[baseHash] = mac;
            net.key = QCryptographicHash::hash(base + '|' + mac, QCryptographicHash::Sha256).toHex();
            return net;
        }
#else
        Net currentNet() {
            return {};
        }
#endif

        // ── lines and the latest object ──────────────────────────────────────

        QJsonObject withNet(QJsonObject geo, const Net &net) {
            geo.insert(QStringLiteral("net_type"), net.type);
            return SanitizeGeo(geo);
        }

        void setLatest(const QJsonObject &geo) {
            QMutexLocker lock(&latestMutex());
            latest() = geo;
        }

        // `key=value ...` in a fixed order; a value with a space is quoted.
        QString describe(const QJsonObject &geo, const QString &trigger) {
            QStringList parts;
            for (const char *key : {"net_type", "country", "region", "asn", "org"}) {
                const QJsonValue v = geo.value(QLatin1String(key));
                if (v.isUndefined()) continue;
                QString text = v.isDouble() ? QString::number(v.toInteger()) : v.toString();
                if (text.contains(u' ')) text = u'"' + text + u'"';
                parts << QLatin1String(key) + u'=' + text;
            }
            parts << QStringLiteral("trigger=") + trigger;
            return parts.join(u' ');
        }

        void writeNetLine(const QString &trigger) {
            const QJsonObject geo = withNet(g().cache.value(g().net.key).geo, g().net);
            setLatest(geo);
            NetEvent(describe(geo, trigger), geo);
        }

        // ── whereami ─────────────────────────────────────────────────────────

        // Straight from the user's network: never our proxy. With TUN on the tunnel takes every
        // socket, so nothing is asked while connected in TUN mode.
        bool mayFetch() {
            if (!g().nam || g().busy || g().net.key.isEmpty()) return false;
            if (settings()->shadowlos_send_journal == 0) return false;
            if (settings()->spmode_vpn && settings()->started_id >= 0) return false;
            QString token;
            return ApiToken(&token);
        }

        void fetch() {
            auto &s = g();
            QString token;
            if (!ApiToken(&token)) return;
            QNetworkRequest request(QUrl(QLatin1String(ApiBase) + QLatin1String(kWhereamiPath)));
            request.setRawHeader("Authorization", "Sl-Sub " + token.toUtf8());
            request.setRawHeader("X-Sl-Install-Id", InstallId().toUtf8());
            request.setRawHeader("X-Sl-Platform", "windows");
            request.setRawHeader("X-Shadowlos-App", kAppHeader);
            request.setTransferTimeout(15 * 1000);
            s.busy = true;
            const QByteArray key = s.net.key;
            const int epoch = s.connectEpoch;
            const bool tun = settings()->spmode_vpn;
            QElapsedTimer clock;
            clock.start();
            auto *reply = s.nam->get(request);
            QObject::connect(reply, &QNetworkReply::finished, s.nam, [reply, key, epoch, tun, clock] {
                reply->deleteLater();
                auto &s = g();
                s.busy = false;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const qint64 now = QDateTime::currentSecsSinceEpoch();
                Entry &entry = s.cache[key];
                // A TUN connect that began meanwhile may have carried the request: not our network.
                const bool tainted = tun && s.connectEpoch != epoch;
                if (status == 200 && !tainted) {
                    const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
                    QJsonObject geo;
                    for (const char *k : {"country", "region", "asn", "org"}) {
                        const QJsonValue v = answer.value(QLatin1String(k));
                        if (!v.isUndefined()) geo.insert(QLatin1String(k), v);
                    }
                    geo = SanitizeGeo(geo);
                    geo.remove(QStringLiteral("net_type"));
                    entry.geo = geo;
                    entry.nextAt = now + kFreshSecs;
                } else {
                    // 404 (an old server), 503 (no databases), 429, a network error: ask again soon.
                    entry.nextAt = now + kRetrySecs;
                }
                Event("api", "call", status == 200 ? "ok" : "fail",
                      QStringLiteral("GET %1 %2").arg(QLatin1String(kWhereamiPath), status ? QString::number(status) : QStringLiteral("error")),
                      status == 200 ? (tainted ? QStringLiteral("dropped: a TUN connect started meanwhile") : QString())
                                    : status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString(),
                      clock.elapsed());
                if (key == s.net.key) setLatest(withNet(entry.geo, s.net));
                if (!s.pendingLine.isEmpty()) {
                    const QString trigger = s.pendingLine;
                    s.pendingLine.clear();
                    writeNetLine(trigger);
                }
            });
        }

        bool fetchDue() {
            const auto it = g().cache.constFind(g().net.key);
            return it == g().cache.constEnd() || QDateTime::currentSecsSinceEpoch() >= it->nextAt;
        }

        // A net line now, or once the answer in flight (or one started here) arrives.
        void lineOrFetch(const QString &trigger) {
            if (g().busy) {
                g().pendingLine = trigger;
                return;
            }
            if (fetchDue() && mayFetch()) {
                g().pendingLine = trigger;
                fetch();
                return;
            }
            writeNetLine(trigger);
        }

        // Every 15 s: a changed network gets a net line; a stale answer is refreshed quietly.
        void check() {
            auto &s = g();
            const Net net = currentNet();
            const bool changed = s.checked && (net.key != s.net.key || net.type != s.net.type);
            s.net = net;
            if (!s.checked) {
                s.checked = true;
                setLatest(withNet(s.cache.value(net.key).geo, net));
            }
            if (changed) {
                lineOrFetch(QStringLiteral("network-change"));
                return;
            }
            if (fetchDue() && mayFetch()) fetch();
        }

    } // namespace

    QJsonObject SanitizeGeo(const QJsonObject &in) {
        static const QRegularExpression country(QStringLiteral("^[A-Z]{2}$"));
        static const QRegularExpression mccMnc(QStringLiteral("^[0-9]{5,6}$"));
        static const QRegularExpression netType(QStringLiteral("^[a-z0-9_-]{1,16}$"));
        QJsonObject out;
        const auto put = [&out](const char *key, const QString &value) {
            if (!value.isEmpty()) out.insert(QLatin1String(key), value);
        };
        put("net_type", shaped(in.value(QStringLiteral("net_type")), netType));
        put("carrier", plainText(in.value(QStringLiteral("carrier")).toString(), 64) ? in.value(QStringLiteral("carrier")).toString() : QString());
        put("mcc_mnc", shaped(in.value(QStringLiteral("mcc_mnc")), mccMnc));
        put("country", shaped(in.value(QStringLiteral("country")), country));
        // No city, ever: an older answer or line that has one loses it here.
        const QString region = in.value(QStringLiteral("region")).toString();
        put("region", plainText(region, 64) ? region : QString());
        const QJsonValue asnValue = in.value(QStringLiteral("asn"));
        const double asnNumber = asnValue.isDouble() ? asnValue.toDouble() : 0;
        const qint64 asn = asnNumber >= 1 && asnNumber <= 4294967295.0 && asnNumber == double(qint64(asnNumber)) ? qint64(asnNumber) : 0;
        if (asn > 0) out.insert(QStringLiteral("asn"), asn);
        // An org that cannot be sent as it is (amazon.com, a link) is named by its AS.
        const QString org = in.value(QStringLiteral("org")).toString();
        if (plainText(org, 128)) put("org", org);
        else if (!org.isEmpty() && asn > 0) put("org", QStringLiteral("AS%1").arg(asn));
        return out;
    }

    QJsonObject LatestGeo() {
        QMutexLocker lock(&latestMutex());
        return latest();
    }

    void SetLatestGeoForTest(const QJsonObject &geo) {
        setLatest(SanitizeGeo(geo));
    }

    void InstallGeo(QObject *owner) {
        auto &s = g();
        s.nam = new QNetworkAccessManager(owner);
        s.nam->setProxy(QNetworkProxy::NoProxy);
        auto *timer = new QTimer(owner);
        timer->setInterval(kCheckEveryMs);
        QObject::connect(timer, &QTimer::timeout, owner, [] { check(); });
        timer->start();
        // The first look (and whereami) at once, the start line 6 s in.
        QTimer::singleShot(0, owner, [] { check(); });
        QTimer::singleShot(kStartLineMs, owner, [] {
            if (!g().checked) check();
            lineOrFetch(QStringLiteral("start"));
        });
    }

    // UI thread. Written at once with what is known: the tunnel may come up any moment.
    void GeoConnectStart() {
        auto &s = g();
        ++s.connectEpoch;
        const Net net = currentNet();
        s.net = net;
        s.checked = true;
        writeNetLine(QStringLiteral("connect"));
    }

} // namespace Shadowlos::Journal

#endif // SHADOWLOS_DEV
