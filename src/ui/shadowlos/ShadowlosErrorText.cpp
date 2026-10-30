#include "include/ui/shadowlos/ShadowlosErrorText.hpp"

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Utils.hpp"

#include <QCoreApplication>
#include <QFont>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
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

        // TUN mode as set now; off before the settings are loaded (the self-test).
        bool TunModeOn() {
            const auto *dm = Configs::dataManager;
            return dm != nullptr && dm->settingsRepo != nullptr && dm->settingsRepo->spmode_vpn;
        }

        struct LastError {
            QMutex mutex;
            QString line;    // the short form
            QString tooltip; // the full sentence
        };

        LastError &last() {
            static LastError l;
            return l;
        }

    } // namespace

    Kind Classify(const QString &raw, int tunOn) {
        const QString e = raw.toLower();
        if (e.trimmed().isEmpty()) return Kind::Unknown;
        if (hasAny(e, {"cache-file", "cache.db", "cache file"})) return Kind::CacheLocked;
        if (hasAny(e, {"rule-set", "rule_set", "ruleset"}) &&
            hasAny(e, {"download", "fetch", "http", "get \"", "dial", "lookup", "timeout", "timed out", "status", "eof", "tls", "connection"}))
            return Kind::RuleSet;
        // "tun" as a word or a known token (tun-in, tun0, wintun), never inside "return" or "tunnel".
        static const QRegularExpression tunWord(QStringLiteral(R"((?<![a-z])tun(?![a-z])|wintun)"));
        const bool tun = tunWord.match(e).hasMatch();
        if (hasAny(e, {"requires elevation", "administrator", "elevation"})) return Kind::AdminRequired;
        // A plain denial is about rights for TUN only when TUN is on or the text is about it;
        // elsewhere (a file, a socket) it is something else.
        if (hasAny(e, {"access is denied", "access denied", "operation not permitted", "permission denied"})) {
            const bool tunMode = tunOn < 0 ? TunModeOn() : tunOn != 0;
            return (tunMode || tun || hasAny(e, {"wfp", "fwpm"})) ? Kind::AdminRequired : Kind::Unknown;
        }
        if (e.contains(QLatin1String("fwpm")) ||
            (tun && hasAny(e, {"already exists", "file exists", "in use by another", "already in use"})))
            return Kind::AnotherVpn;
        if (tun && hasAny(e, {"configure tun interface", "open tun", "create tun", "wintun", "tun device", "tun interface"}))
            return Kind::TunFailed;
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
        // "Operation canceled" is Qt's word for any abort, not only a timeout: left unknown.
        if (hasAny(e, {"timeout", "timed out", "deadline exceeded"})) return Kind::Timeout;
        if (hasAny(e, {"connection reset", "forcibly closed", "broken pipe", "unexpected eof", "remote host closed",
                       "connection closed"}))
            return Kind::Reset;
        // "status code 403", "HTTP 403", "HTTP/1.1 502", "HTTP status 404": never a port or a URL.
        static const QRegularExpression http(QStringLiteral(R"(\bstatus code:? [45]\d\d\b|\bhttp(?:/[\d.]+)?(?: status)?:? [45]\d\d\b)"));
        if (hasAny(e, {"server replied", "unexpected status"}) || http.match(e).hasMatch()) return Kind::ServerHttp;
        if (hasAny(e, {"ipc", "rpc", "core is not running", "core is unreachable"})) return Kind::CoreDown;
        return Kind::Unknown;
    }

    QString Sentence(const QString &raw, const QString &fallback) {
        switch (Classify(raw)) {
            case Kind::AdminRequired:
                return translate("Administrator rights are needed for TUN mode. Run the app as administrator or turn TUN mode off.");
            case Kind::AnotherVpn:
                return translate("ShadowLos or another VPN may be running twice. Close all copies of ShadowLos and other VPNs, then start ShadowLos again.");
            case Kind::TunFailed:
                return translate("Could not turn on TUN mode. Close other VPN apps, restart the app and try again.");
            case Kind::PortInUse: {
                const QString port = portOf(raw);
                if (!port.isEmpty())
                    return translate("ShadowLos may be running twice. Port %1 is already in use. Close the program that uses it (for example, a second copy of ShadowLos) or change the port in Settings.").arg(port);
                return translate("ShadowLos may be running twice. A port the app needs is already in use. Close the program that uses it (for example, a second copy of ShadowLos) or change the port in Settings.");
            }
            case Kind::CacheLocked:
                return translate("ShadowLos may be running twice. Close all copies of ShadowLos and start it again.");
            case Kind::RuleSet:
                return translate("Could not download the GeoIP database (the region lists for routing). Check the internet connection and try again.");
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

    QString Short(const QString &raw, const QString &fallback) {
        switch (Classify(raw)) {
            case Kind::AdminRequired:
                return translate("TUN mode needs administrator rights");
            case Kind::AnotherVpn:
                return translate("ShadowLos or a VPN runs twice. Close the extra ones");
            case Kind::TunFailed:
                return translate("TUN mode failed. Restart the app");
            case Kind::PortInUse: {
                const QString port = portOf(raw);
                if (!port.isEmpty()) return translate("Port %1 is busy. ShadowLos may run twice").arg(port);
                return translate("A port is busy. ShadowLos may run twice");
            }
            case Kind::CacheLocked:
                return translate("ShadowLos runs twice. Close the extra copies");
            case Kind::RuleSet:
                return translate("GeoIP database did not download. Check the internet");
            case Kind::Dns:
                return translate("DNS error. Check the internet or change server");
            case Kind::Tls:
                return translate("Secure connection failed. Change server");
            case Kind::Timeout:
                return translate("The server does not answer. Change server");
            case Kind::Refused:
                return translate("The server refused. Change server");
            case Kind::Reset:
                return translate("Connection cut off. Check the internet");
            case Kind::NetworkDown:
                return translate("No internet. Check the network");
            case Kind::ServerHttp:
                return translate("The server answered with an error. Try later");
            case Kind::CoreDown:
                return translate("The core does not answer. Restart the app");
            case Kind::AlreadyRunning:
                return translate("Already connected. Disconnect and connect again");
            case Kind::Unknown:
                break;
        }
        return fallback;
    }

    void AttachRaw(QMessageBox *box, const QString &raw) {
        const QString text = raw.trimmed();
        if (box == nullptr || text.isEmpty()) return;
        // An informative text gives the box a row under its text (the buttons move below it);
        // that row holds the raw text instead of the informative label.
        box->setInformativeText(QStringLiteral(" "));
        if (auto *informative = box->findChild<QLabel *>(QStringLiteral("qt_msgbox_informativelabel"))) informative->hide();
        auto *grid = qobject_cast<QGridLayout *>(box->layout());
        if (grid == nullptr) return;
        // Small and selectable, in an area of at most about eight lines that scrolls: a rule-set
        // error can run to pages and must not make the window huge. No colour of its own.
        auto *label = new QLabel(text);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() * 0.85);
        label->setFont(font);
        auto *area = new QScrollArea(box);
        area->setObjectName(QStringLiteral("slErrorRaw"));
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setStyleSheet(QStringLiteral("QScrollArea#slErrorRaw, QScrollArea#slErrorRaw > QWidget > QWidget { background: transparent; }"));
        area->setWidget(label);
        constexpr int kWidth = 480;
        const int lines = label->fontMetrics().lineSpacing();
        const int cap = lines * 8;
        const int needed = label->heightForWidth(kWidth);
        const bool scrolls = needed > cap;
        area->setVerticalScrollBarPolicy(scrolls ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
        area->setFixedSize(kWidth + (scrolls ? area->verticalScrollBar()->sizeHint().width() : 0),
                           scrolls ? cap : qMax(needed, lines) + 2);
        grid->addWidget(area, 1, 1, 1, 1);
    }

    int ShowError(const QString &title, const QString &sentence, const QString &raw) {
        QMessageBox box(QMessageBox::Warning, title, sentence, QMessageBox::Ok, GetMessageBoxParent());
        AttachRaw(&box, raw);
        return box.exec();
    }

    QString ConnectFallback() {
        return translate("Could not connect. Try another server or restart the app.");
    }

    QString ConnectFallbackShort() {
        return translate("Could not connect. Change server");
    }

    QString ConnectTitle() {
        return translate("Could not connect");
    }

    void SetConnectError(const QString &raw) {
        SetConnectErrorLine(Short(raw, ConnectFallbackShort()), Sentence(raw, ConnectFallback()));
    }

    void SetConnectErrorLine(const QString &shortLine, const QString &full) {
        auto &l = last();
        QMutexLocker lock(&l.mutex);
        l.line = shortLine;
        l.tooltip = full;
    }

    void ClearConnectError() {
        SetConnectErrorLine({}, {});
    }

    QString ConnectErrorLine() {
        auto &l = last();
        QMutexLocker lock(&l.mutex);
        return l.line;
    }

    QString ConnectErrorTooltip() {
        auto &l = last();
        QMutexLocker lock(&l.mutex);
        return l.tooltip;
    }

    namespace {
        // Real wording from sing-box, xray, Windows and Qt; fake addresses only.
        const struct {
            const char *raw;
            Kind want;
            const char *demo; // the SHADOWLOS_UI_DEMO token that shows it, or nullptr
            int tun = 0;      // TUN mode on while it happened
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
            // The first download of the GeoIP rule-sets, failing all at once: a long text.
            {"start service: initialize rule-set failed:\n"
             "initialize rule-set[geoip-ru]: Get \"https://geo.example.com/geoip-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-category-ru]: Get \"https://geo.example.com/geosite-category-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-category-gov-ru]: Get \"https://geo.example.com/geosite-category-gov-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-category-retail-ru]: Get \"https://geo.example.com/geosite-category-retail-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-category-education-ru]: Get \"https://geo.example.com/geosite-category-education-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geoip-private]: Get \"https://geo.example.com/geoip-private.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-ru-available-only-inside]: Get \"https://geo.example.com/geosite-ru-available-only-inside.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-category-bank-ru]: Get \"https://geo.example.com/geosite-category-bank-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-category-media-ru]: Get \"https://geo.example.com/geosite-category-media-ru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-yandex]: Get \"https://geo.example.com/geosite-yandex.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-vk]: Get \"https://geo.example.com/geosite-vk.srs\": dial tcp 203.0.113.7:443: i/o timeout\n"
             "initialize rule-set[geosite-mailru]: Get \"https://geo.example.com/geosite-mailru.srs\": dial tcp 203.0.113.7:443: i/o timeout\n",
             Kind::RuleSet, "err-geoip"},
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
            // A plain denial means admin rights only with TUN on or about TUN/WFP.
            {"open C:/Users/x/config/geoip.db: Access is denied.", Kind::Unknown, nullptr, 0},
            {"open C:/Users/x/config/geoip.db: Access is denied.", Kind::AdminRequired, nullptr, 1},
            {"listen udp 0.0.0.0:53: bind: permission denied", Kind::Unknown, nullptr, 0},
            {"FwpmEngineOpen0: Access is denied.", Kind::AdminRequired, nullptr, 0},
            {"wfp: operation not permitted", Kind::AdminRequired, nullptr, 0},
            // "tun" as a word only: not inside "return" or "tunnel".
            {"tunnel returned: object already exists", Kind::Unknown, nullptr},
            {"open tun0: file exists", Kind::AnotherVpn, nullptr},
            {"wintun: adapter already exists", Kind::AnotherVpn, nullptr},
            // Qt's "Operation canceled" is any abort, not a timeout.
            {"Operation canceled", Kind::Unknown, nullptr},
            // An HTTP status only after "status code" or "HTTP", never a port or a URL.
            {"dial tcp 203.0.113.7:404: connectex: an attempt was made", Kind::Unknown, nullptr},
            {"GET http://example.com:8443/sub/503 failed", Kind::Unknown, nullptr},
            {"subscription: unexpected response, status code 502", Kind::ServerHttp, nullptr},
            {"Download failed: server returned HTTP status 403.", Kind::ServerHttp, nullptr},
            {"HTTP/1.1 404 Not Found", Kind::ServerHttp, nullptr},
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
            const Kind got = Classify(raw, c.tun);
            if (got != c.want) problems << QStringLiteral("kind %1 want %2: %3").arg(int(got)).arg(int(c.want)).arg(raw);
            // Every known kind has both forms; neither ever carries the raw text.
            if (c.want != Kind::Unknown && c.tun == 0) {
                const QString full = Sentence(raw), brief = Short(raw);
                if (full.isEmpty() || brief.isEmpty()) problems << QStringLiteral("no text: ") + raw;
                if (brief.size() > 55) problems << QStringLiteral("short form too long: ") + brief;
                if (raw.size() > 12 && (full.contains(raw) || brief.contains(raw))) problems << QStringLiteral("raw shown: ") + raw;
            }
        }
        if (portOf(QString::fromUtf8(kCases[0].raw)) != QStringLiteral("2080")) problems << QStringLiteral("port not found");
        if (!Short(QString::fromUtf8(kCases[0].raw)).contains(QStringLiteral("2080"))) problems << QStringLiteral("short form lost the port");
        if (!Sentence(QStringLiteral("something new")).isEmpty() || !Short(QStringLiteral("something new")).isEmpty())
            problems << QStringLiteral("unknown has a text");
        if (Sentence(QStringLiteral("something new"), QStringLiteral("x")) != QStringLiteral("x") ||
            Short(QStringLiteral("something new"), QStringLiteral("y")) != QStringLiteral("y"))
            problems << QStringLiteral("fallback lost");
        if (ConnectFallbackShort().size() > 55) problems << QStringLiteral("fallback short form too long");
        SetConnectError(QString::fromUtf8(kCases[2].raw));
        const bool set = !ConnectErrorLine().isEmpty() && ConnectErrorTooltip().size() > ConnectErrorLine().size();
        ClearConnectError();
        if (!set || !ConnectErrorLine().isEmpty() || !ConnectErrorTooltip().isEmpty()) problems << QStringLiteral("connect error line");
        return problems.join(QStringLiteral("; "));
    }

} // namespace Shadowlos::ErrorText
