#include "include/ui/shadowlos/ShadowlosErrorText.hpp"

#include <QCoreApplication>
#include <QMutex>
#include <QRegularExpression>
#include <QStringList>

#include <initializer_list>
#include <iterator>

namespace Shadowlos::ErrorText {

    namespace {

        QString translate(const char *text) {
            return QCoreApplication::translate("ShadowlosErrorText", text);
        }

        bool hasAny(const QString &lower, std::initializer_list<const char *> words) {
            for (const char *w : words) {
                if (lower.contains(QLatin1String(w))) return true;
            }
            return false;
        }

        // The port of a failed listen: "listen tcp 127.0.0.1:2080: bind: ...".
        QString portOf(const QString &raw) {
            static const QRegularExpression re(QStringLiteral(R"((?:listen \S+ \S*:|:)(\d{2,5}): ?bind)"));
            const auto m = re.match(raw);
            return m.hasMatch() ? m.captured(1) : QString();
        }

        struct LastError {
            QMutex mutex;
            QString line;
        };

        LastError &last() {
            static LastError l;
            return l;
        }

    } // namespace

    Kind Classify(const QString &raw) {
        const QString e = raw.toLower();
        if (e.trimmed().isEmpty()) return Kind::Unknown;
        if (hasAny(e, {"cache-file", "cache.db", "cache file"})) return Kind::CacheLocked;
        if (hasAny(e, {"rule-set", "rule_set", "ruleset"}) &&
            hasAny(e, {"download", "fetch", "http", "get \"", "dial", "lookup", "timeout", "timed out", "status", "eof", "tls", "connection"}))
            return Kind::RuleSet;
        if (hasAny(e, {"access is denied", "requires elevation", "operation not permitted", "permission denied",
                       "administrator", "elevation"}))
            return Kind::AdminRequired;
        const bool tun = hasAny(e, {"tun", "wintun"});
        if (e.contains(QLatin1String("fwpm")) ||
            (tun && hasAny(e, {"already exists", "file exists", "in use by another", "already in use"})))
            return Kind::AnotherVpn;
        if (hasAny(e, {"configure tun interface", "open tun", "create tun", "wintun", "tun device"})) return Kind::TunFailed;
        if (hasAny(e, {"address already in use", "only one usage of each socket address", "wsaeaddrinuse", ": bind:"}))
            return Kind::PortInUse;
        if (e.contains(QLatin1String("instance already started"))) return Kind::AlreadyRunning;
        if (hasAny(e, {"network is unreachable", "network unreachable", "unreachable network", "no route to host",
                       "network is down", "host is unreachable"}))
            return Kind::NetworkDown;
        static const QRegularExpression qtHost(QStringLiteral(R"(host \S+ not found)"));
        if (hasAny(e, {"no such host", "lookup ", "dns", "server misbehaving", "name resolution", "could not resolve",
                       "no address associated"}) ||
            qtHost.match(e).hasMatch())
            return Kind::Dns;
        if (hasAny(e, {"x509", "certificate", "tls", "handshake", "reality", "ssl"})) return Kind::Tls;
        if (hasAny(e, {"connection refused", "actively refused", "no connection could be made"})) return Kind::Refused;
        if (hasAny(e, {"timeout", "timed out", "deadline exceeded", "operation canceled"})) return Kind::Timeout;
        if (hasAny(e, {"connection reset", "forcibly closed", "broken pipe", "unexpected eof", "remote host closed",
                       "connection closed"}))
            return Kind::Reset;
        static const QRegularExpression http(QStringLiteral(R"(\bhttp\b.*\b[45]\d\d\b|\bstatus(?: code)?:? [45]\d\d\b)"));
        if (hasAny(e, {"server replied", "unexpected status"}) || http.match(e).hasMatch()) return Kind::ServerHttp;
        if (hasAny(e, {"ipc", "rpc", "core is not running", "core is unreachable"})) return Kind::CoreDown;
        return Kind::Unknown;
    }

    QString Sentence(const QString &raw, const QString &fallback) {
        switch (Classify(raw)) {
            case Kind::AdminRequired:
                return translate("Administrator rights are needed for TUN mode. Run the app as administrator or turn TUN mode off.");
            case Kind::AnotherVpn:
                return translate("Another VPN is in the way. Close the other VPN and try again.");
            case Kind::TunFailed:
                return translate("Could not turn on TUN mode. Close other VPN apps, restart the app and try again.");
            case Kind::PortInUse: {
                const QString port = portOf(raw);
                if (!port.isEmpty())
                    return translate("Port %1 is already in use. Close the program that uses it (for example, a second copy of ShadowLos) or change the port in Settings.").arg(port);
                return translate("A port the app needs is already in use. Close the program that uses it (for example, a second copy of ShadowLos) or change the port in Settings.");
            }
            case Kind::CacheLocked:
                return translate("The cache file is busy in another copy of the app. Close the second copy of ShadowLos and try again.");
            case Kind::RuleSet:
                return translate("Could not download the routing rule lists. Check the internet connection and try again.");
            case Kind::Dns:
                return translate("Could not find the server address (DNS error). Check the internet connection or pick another server.");
            case Kind::Tls:
                return translate("Could not set up a secure connection to the server. Pick another server or update the subscription.");
            case Kind::Timeout:
                return translate("The server did not answer in time. Check the internet connection or pick another server.");
            case Kind::Refused:
                return translate("The server refused the connection. Pick another server or update the subscription.");
            case Kind::Reset:
                return translate("The connection was cut off. Check the internet connection and try again.");
            case Kind::NetworkDown:
                return translate("No internet connection. Check the network and try again.");
            case Kind::ServerHttp:
                return translate("The server answered with an error. Try again later or update the subscription.");
            case Kind::CoreDown:
                return translate("The app's core is not responding. Restart the app.");
            case Kind::AlreadyRunning:
                return translate("A connection is already running. Disconnect and connect again.");
            case Kind::Unknown:
                break;
        }
        return fallback;
    }

    QString Html(const QString &raw, const QString &fallback) {
        const QString sentence = Sentence(raw, fallback);
        QString original = raw.trimmed().toHtmlEscaped();
        original.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        if (sentence.isEmpty()) return QStringLiteral("<p>%1</p>").arg(original);
        if (original.isEmpty()) return QStringLiteral("<p>%1</p>").arg(sentence.toHtmlEscaped());
        return QStringLiteral("<p>%1</p><p><small>%2</small></p>").arg(sentence.toHtmlEscaped(), original);
    }

    QString Plain(const QString &raw) {
        const QString sentence = Sentence(raw);
        if (sentence.isEmpty()) return raw;
        return QStringLiteral("%1 (%2)").arg(sentence, raw.trimmed());
    }

    QString ConnectFallback() {
        return translate("Could not connect. Try another server or restart the app.");
    }

    QString ConnectTitle() {
        return translate("Could not connect");
    }

    void SetConnectError(const QString &raw) {
        auto &l = last();
        QMutexLocker lock(&l.mutex);
        l.line = Sentence(raw, ConnectFallback());
    }

    void ClearConnectError() {
        auto &l = last();
        QMutexLocker lock(&l.mutex);
        l.line.clear();
    }

    QString ConnectErrorLine() {
        auto &l = last();
        QMutexLocker lock(&l.mutex);
        return l.line;
    }

    namespace {
        // Real wording from sing-box, xray, Windows and Qt; fake addresses only.
        const struct {
            const char *raw;
            Kind want;
            const char *demo; // the SHADOWLOS_UI_DEMO token that shows it, or nullptr
        } kCases[] = {
            {"start service: start inbound/mixed[mixed-in]: listen tcp 127.0.0.1:2080: bind: Only one usage of each socket address (protocol/network address/port) is normally permitted.", Kind::PortInUse, "err-port"},
            {"listen tcp 0.0.0.0:2080: bind: address already in use", Kind::PortInUse, nullptr},
            {"start service: start inbound/tun[tun-in]: configure tun interface: Access is denied.", Kind::AdminRequired, "err-admin"},
            {"configure tun interface: The requested operation requires elevation.", Kind::AdminRequired, nullptr},
            {"start service: start inbound/tun[tun-in]: configure tun interface: Cannot create a file when that file already exists.", Kind::AnotherVpn, "err-vpn"},
            {"start service: start inbound/tun[tun-in]: FwpmFilterAdd0: The filter already exists.", Kind::AnotherVpn, nullptr},
            {"start service: start inbound/tun[tun-in]: configure tun interface: The system cannot find the file specified.", Kind::TunFailed, nullptr},
            {"start service: initialize cache-file: timeout", Kind::CacheLocked, "err-cache"},
            {"open C:\\Users\\x\\config\\cache.db: The process cannot access the file because it is being used by another process.", Kind::CacheLocked, nullptr},
            {"initialize rule-set[geoip-ru]: Get \"https://example.com/geoip-ru.srs\": dial tcp: lookup example.com: no such host", Kind::RuleSet, "err-ruleset"},
            {"initialize rule-set[geosite-ru]: unexpected status: 404 Not Found", Kind::RuleSet, nullptr},
            {"dial tcp: lookup node1.example.com: no such host", Kind::Dns, "err-dns"},
            {"Host sub.example.com not found", Kind::Dns, nullptr},
            {"remote error: tls: handshake failure", Kind::Tls, "err-tls"},
            {"x509: certificate signed by unknown authority", Kind::Tls, nullptr},
            {"dial tcp 203.0.113.7:443: i/o timeout", Kind::Timeout, "err-timeout"},
            {"context deadline exceeded", Kind::Timeout, nullptr},
            {"dial tcp 203.0.113.7:443: connectex: No connection could be made because the target machine actively refused it.", Kind::Refused, nullptr},
            {"Connection refused", Kind::Refused, nullptr},
            {"read tcp 10.0.0.2:5000->203.0.113.7:443: wsarecv: An existing connection was forcibly closed by the remote host.", Kind::Reset, nullptr},
            {"dial udp 203.0.113.7:443: connect: network is unreachable", Kind::NetworkDown, nullptr},
            {"Error transferring https://sub.example.com/x - server replied: Forbidden", Kind::ServerHttp, nullptr},
            {"IPC call failed (code 2)", Kind::CoreDown, nullptr},
            {"instance already started", Kind::AlreadyRunning, nullptr},
            {"json: cannot unmarshal string into Go value of type option.Listable", Kind::Unknown, nullptr},
            {"", Kind::Unknown, nullptr},
        };
    } // namespace

    QString DemoRaw(const QString &token) {
        for (const auto &c : kCases) {
            if (c.demo != nullptr && token == QLatin1String(c.demo)) return QString::fromUtf8(c.raw);
        }
        return {};
    }

    QString SelfTest() {
        QStringList problems;
        for (const auto &c : kCases) {
            const QString raw = QString::fromUtf8(c.raw);
            const Kind got = Classify(raw);
            if (got != c.want) problems << QStringLiteral("kind %1 want %2: %3").arg(int(got)).arg(int(c.want)).arg(raw);
        }
        if (portOf(QString::fromUtf8(kCases[0].raw)) != QStringLiteral("2080")) problems << QStringLiteral("port not found");
        // Every known kind has a sentence; unknown has the fallback only; the original is kept.
        const QString html = Html(QString::fromUtf8(kCases[0].raw));
        if (!html.contains(QStringLiteral("<small>")) || !html.contains(QStringLiteral("2080: bind")))
            problems << QStringLiteral("html keeps no original: ") + html;
        if (!Sentence(QStringLiteral("something new")).isEmpty()) problems << QStringLiteral("unknown has a sentence");
        if (Sentence(QStringLiteral("something new"), QStringLiteral("x")) != QStringLiteral("x")) problems << QStringLiteral("fallback lost");
        if (Plain(QStringLiteral("something new")) != QStringLiteral("something new")) problems << QStringLiteral("plain changed unknown");
        if (Html(QStringLiteral("<b>&")).contains(QStringLiteral("<b>"))) problems << QStringLiteral("html not escaped");
        SetConnectError(QString::fromUtf8(kCases[2].raw));
        const bool set = !ConnectErrorLine().isEmpty();
        ClearConnectError();
        if (!set || !ConnectErrorLine().isEmpty()) problems << QStringLiteral("connect error line");
        return problems.join(QStringLiteral("; "));
    }

} // namespace Shadowlos::ErrorText
