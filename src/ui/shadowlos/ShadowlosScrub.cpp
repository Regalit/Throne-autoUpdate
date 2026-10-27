#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#if SHADOWLOS_DEV

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <cstdio>
#include <functional>

namespace Shadowlos::Journal {

    namespace {

        using Rx = QRegularExpression;
        constexpr auto CI = Rx::CaseInsensitiveOption;

        // Calls `replace` for each match; it returns the text to put in its place.
        QString replaceEach(const QString &s, const Rx &re, const std::function<QString(const QRegularExpressionMatch &)> &replace) {
            QString out;
            qsizetype last = 0;
            auto it = re.globalMatch(s);
            while (it.hasNext()) {
                const auto m = it.next();
                out += QStringView(s).mid(last, m.capturedStart() - last);
                out += replace(m);
                last = m.capturedEnd();
            }
            out += QStringView(s).mid(last);
            return out;
        }

        // "Host-looking" tokens that are really source files (main.dart:42, mainwindow.cpp:120):
        // kept, they are what a stack trace is made of.
        bool isSourceExt(QStringView ext) {
            static const QSet<QString> exts{"dart", "kt", "java", "go", "so", "js", "c", "cc", "cpp",
                                            "h", "hpp", "py", "xml", "json"};
            return exts.contains(ext.toString().toLower());
        }

        // Name endings that are files, not top-level domains (ThroneCore.exe, throne.db, geoip.dat).
        bool isFileExt(QStringView ext) {
            static const QSet<QString> exts{"exe", "dll", "sys", "log", "txt", "jsonl", "db", "dat", "dmp", "png", "jpg",
                                            "ico", "svg", "qss", "qrc", "qm", "ts", "ini", "cfg", "conf", "yaml", "yml",
                                            "toml", "md", "bat", "cmd", "ps1", "sh", "zip", "gz", "tmp", "lock", "pid",
                                            "marker", "rs", "cs", "hxx", "cxx", "mm", "kts", "proto", "srs"};
            return isSourceExt(ext) || exts.contains(ext.toString().toLower());
        }

        // A last label in mixed case is a dotted identifier (java.net.SocketException,
        // QApplication.MainWindow), not a host name: those are written in one case.
        bool isIdentifierLabel(QStringView label) {
            bool upper = false, lower = false;
            for (const QChar c : label) {
                if (c.isUpper()) upper = true;
                else if (c.isLower()) lower = true;
            }
            return upper && lower;
        }

        QStringView extOf(QStringView name) {
            return name.mid(name.lastIndexOf(u'.') + 1);
        }

        bool looksLikeKey(QStringView m) {
            bool digit = false, upper = false, lower = false;
            for (const QChar c : m) {
                if (c >= u'0' && c <= u'9') digit = true;
                else if (c >= u'A' && c <= u'Z') upper = true;
                else if (c >= u'a' && c <= u'z') lower = true;
            }
            return digit && upper && lower;
        }

    } // namespace

    // Order matters: links first (they carry everything else), then tokens and key=value
    // secrets, then the shapes of addresses and keys.
    QString Scrub(const QString &text) {
        static const Rx uri(QStringLiteral(R"(\b([a-z][a-z0-9+.-]{1,15})://[^\s"'<>]+)"), CI);
        static const Rx jwt(QStringLiteral(R"(\beyJ[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]*)"));
        static const Rx bearer(QStringLiteral(R"(\bbearer\s+[A-Za-z0-9._~+/=-]+)"), CI);
        static const Rx keyValue(QStringLiteral(
            R"(\b(password|passwd|pwd|pass|token|secret|psk|private_key|public_key|key|auth|authorization|cookie|uuid|sid|pbk|sni|host|server|address|addr|telegram_id|tg_id|tgid|user_id|userid|user_id_hash)("?\s*[=:]\s*)("[^"]*"|[^\s,;&"']+))"), CI);
        static const Rx uuid(QStringLiteral(R"(\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b)"), CI);
        static const Rx email(QStringLiteral(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"));
        static const Rx ipv4(QStringLiteral(R"(\b(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}(?::\d{1,5})?\b)"));
        static const Rx ipv6(QStringLiteral(R"([0-9a-f]{0,4}(?::[0-9a-f]{0,4}){2,7})"), CI);
        static const Rx hostPort(QStringLiteral(R"(\b((?:[a-z0-9-]+\.)+[a-z][a-z0-9-]{1,62}):(\d{2,5})\b)"), CI);
        // Any dotted name whose last label is 2-24 letters, or a punycode one (xn--p1ai).
        static const Rx domain(QStringLiteral(R"(\b(?:[a-z0-9-]+\.)+(?:xn--[a-z0-9-]{2,59}|[a-z]{2,24})\b)"), CI);
        // Lookarounds, not \b: hex glued to a word (hash_<64 hex>) is still a key.
        static const Rx hex(QStringLiteral(R"((?<![0-9a-f])[0-9a-f]{32,}(?![0-9a-f]))"), CI);
        // \Users\<name>\ in either slash style; a name with spaces only up to the next slash.
        static const Rx userDir(QStringLiteral(R"(([\\/])Users([\\/])(?:[^\\/"'<>\r\n]{1,64}?(?=[\\/])|[^\\/\s"'<>]+))"), CI);
        static const Rx base64(QStringLiteral(R"([A-Za-z0-9+/_-]{32,}={0,2})"));

        QString s = text;
        s.replace(userDir, QStringLiteral("\\1Users\\2<user>"));
        s.replace(uri, QStringLiteral("\\1://<redacted>"));
        s.replace(jwt, QStringLiteral("<jwt>"));
        s.replace(bearer, QStringLiteral("Bearer <redacted>"));
        s.replace(keyValue, QStringLiteral("\\1\\2<redacted>"));
        s.replace(uuid, QStringLiteral("<uuid>"));
        s.replace(email, QStringLiteral("<email>"));
        s.replace(ipv4, QStringLiteral("<ip>"));
        // IPv6 / MAC: 4+ hex digits and either 4+ colons or a "::". Times (12:34:56) and
        // String::class stay.
        s = replaceEach(s, ipv6, [](const QRegularExpressionMatch &m) {
            const QString t = m.captured();
            const int colons = int(t.count(u':'));
            if (t.size() - colons >= 4 && (colons >= 4 || t.contains(QStringLiteral("::")))) return QStringLiteral("<ip>");
            return t;
        });
        s = replaceEach(s, hostPort, [](const QRegularExpressionMatch &m) {
            if (isSourceExt(extOf(m.capturedView(1)))) return m.captured();
            return QStringLiteral("<host>:") + m.captured(2);
        });
        // Kept: a match followed by ".<letter>" (a dotted identifier cut short), one ending in a
        // file extension (dart_vm_initializer.cc, ThroneCore.exe) or in a mixed-case label.
        s = replaceEach(s, domain, [&s](const QRegularExpressionMatch &m) {
            const qsizetype end = m.capturedEnd();
            if (end + 1 < s.size() && s.at(end) == u'.' && s.at(end + 1).isLetter()) return m.captured();
            const QStringView last = extOf(m.capturedView());
            if (isFileExt(last) || isIdentifierLabel(last)) return m.captured();
            return QStringLiteral("<host>");
        });
        s.replace(hex, QStringLiteral("<hex>"));
        // Only a mix of digits, upper and lower case: long paths and identifiers never have all three.
        s = replaceEach(s, base64, [](const QRegularExpressionMatch &m) {
            return looksLikeKey(m.capturedView()) ? QStringLiteral("<secret>") : m.captured();
        });
        return s;
    }

    // Decode, scrub every string, encode again in the journal's key order: scrubbing the raw
    // text could cut an escaped quote in half and leave a line that no longer parses.
    QString ScrubJsonLine(const QString &line) {
        QJsonParseError error{};
        const auto doc = QJsonDocument::fromJson(line.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return Scrub(line);
        QJsonObject obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isString() && it.key() != QStringLiteral("ts")) it.value() = Scrub(it.value().toString());
        }
        // The scrub above sees top-level strings only: the net line's nested geo object is
        // checked key by key instead, and any other nested value is dropped rather than sent
        // unscrubbed.
        for (const QString &key : obj.keys()) {
            const QJsonValue v = obj.value(key);
            if (key == QStringLiteral("geo")) {
                const QJsonObject geo = SanitizeGeo(v.toObject());
                if (geo.isEmpty()) obj.remove(key);
                else obj.insert(key, geo);
            } else if (v.isObject() || v.isArray()) {
                obj.remove(key);
            }
        }
        QString out = QStringLiteral("{");
        const auto append = [&](const QString &key) {
            if (!obj.contains(key)) return;
            if (out.size() > 1) out += u',';
            const QJsonValue v = obj.take(key);
            // A one-element array is the shortest way to get Qt's own escaping of a value.
            QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
            out += QStringLiteral("\"") + key + QStringLiteral("\":") + encoded.mid(1, encoded.size() - 2);
        };
        for (const char *key : {"ts", "sid", "seq", "layer", "step", "outcome", "ms", "err", "d"}) append(QString::fromLatin1(key));
        for (const QString &key : obj.keys()) append(key);
        return out + u'}';
    }

    namespace {

        void print(const QString &text) {
            const QByteArray bytes = (text + u'\n').toUtf8();
#ifdef Q_OS_WIN
            // A GUI exe has no console: write to an inherited handle (redirected output) or
            // the console of whoever started it.
            HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            if (out == nullptr || out == INVALID_HANDLE_VALUE) {
                static HANDLE console = [] {
                    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return INVALID_HANDLE_VALUE;
                    return CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                }();
                out = console;
            }
            if (out != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(out, bytes.constData(), DWORD(bytes.size()), &written, nullptr);
            }
#else
            fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
            fflush(stdout);
#endif
        }

    } // namespace

    int RunSelfTestIfAsked(const QStringList &args) {
        if (!args.contains(QStringLiteral("--journal-selftest"))) return -1;
        // Fake values only.
        const struct { const char *in; const char *want; } cases[] = {
            {"GET https://sub.example.com/dynamic_config?user_id_hash=abc failed", "GET https://<redacted> failed"},
            {"import vless://id@host:443?sni=x#name done", "import vless://<redacted> done"},
            {"jwt eyJhbGciOiJI.eyJzdWIiOiIx.c2lnbmF0dXJl", "jwt <jwt>"},
            {"sent Bearer abc.DEF-123", "sent Bearer <redacted>"},
            {"password=hunter2 next", "password=<redacted> next"},
            {"user_id_hash: 0000000000000000000000000000000000000000000000000000000000000000", "user_id_hash: <redacted>"},
            {"sni=\"cdn.example.net\", ok", "sni=<redacted>, ok"},
            {"id 123e4567-e89b-12d3-a456-426614174000 ok", "id <uuid> ok"},
            {"mail ivan.petrov@mail.ru now", "mail <email> now"},
            {"dial tcp 203.0.113.7:443: i/o timeout", "dial tcp <ip>: i/o timeout"},
            {"dial [2001:db8::1]:443", "dial [<ip>]:443"},
            {"mac aa:bb:cc:dd:ee:ff", "mac <ip>"},
            {"at 12:34:56 String::class", "at 12:34:56 String::class"},
            {"connect cdn.example.org:8443 refused", "connect <host>:8443 refused"},
            {"lib/main.dart:42 mainwindow.cpp:120", "lib/main.dart:42 mainwindow.cpp:120"},
            {"resolve shadowlos.xyz failed", "resolve <host> failed"},
            {"java.net.SocketException in dart_vm_initializer.cc(41) ThroneCore.exe", "java.net.SocketException in dart_vm_initializer.cc(41) ThroneCore.exe"},
            {"sha 0123456789abcdef0123456789abcdef", "sha <hex>"},
            {"blob AbcdefGhijklmnopqrstuvwxyz0123456789", "blob <secret>"},
            {"io/flutter/embedding/engine/FlutterJNI", "io/flutter/embedding/engine/FlutterJNI"},
            {"Не удалось подключиться v1.0.4-dev build 10004", "Не удалось подключиться v1.0.4-dev build 10004"},
            // Review fixes: any TLD, JSON keys, hex glued to a word, the Windows user name.
            {"lookup node1.example.de: no such host", "lookup <host>: no such host"},
            {"via edge.example.co.uk and xn--80ak6aa92e.xn--p1ai", "via <host> and <host>"},
            {"ShadowlosJournal.cpp 10.0.26200 v1.0.4-dev ThroneCore.exe geoip.dat throne.db QApplication.MainWindow",
             "ShadowlosJournal.cpp 10.0.26200 v1.0.4-dev ThroneCore.exe geoip.dat throne.db QApplication.MainWindow"},
            {R"({"password":"x","server": "h"})", R"({"password":<redacted>,"server": <redacted>})"},
            {"hash_abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789 end", "hash_<hex> end"},
            {R"(C:\Users\Ivan\AppData\Local\x)", R"(C:\Users\<user>\AppData\Local\x)"},
            {R"(C:/Users/ivan.petrov/AppData and C:\Users\John Smith\Desktop)", R"(C:/Users/<user>/AppData and C:\Users\<user>\Desktop)"},
        };
        int failed = 0;
        for (const auto &c : cases) {
            const QString in = QString::fromUtf8(c.in), want = QString::fromUtf8(c.want);
            const QString got = Scrub(in);
            if (got != want) ++failed;
            print((got == want ? QStringLiteral("PASS  ") : QStringLiteral("FAIL  ")) + in +
                  (got == want ? QString() : QStringLiteral("\n      want: ") + want + QStringLiteral("\n      got:  ") + got));
        }
        // A link before an escaped quote must leave a line that still parses.
        const QString line = QStringLiteral(R"({"ts":"2026-10-26T12:00:00.100Z","sid":4,"seq":7,"layer":"core-go","step":"ThroneCore","outcome":"fail","err":"Get \"https://x.example.com/a\": timeout"})");
        const QString wantLine = QStringLiteral(R"({"ts":"2026-10-26T12:00:00.100Z","sid":4,"seq":7,"layer":"core-go","step":"ThroneCore","outcome":"fail","err":"Get \"https://<redacted>\": timeout"})");
        const QString gotLine = ScrubJsonLine(line);
        const bool lineOk = gotLine == wantLine && QJsonDocument::fromJson(gotLine.toUtf8()).isObject();
        if (!lineOk) ++failed;
        print((lineOk ? QStringLiteral("PASS  json line: ") : QStringLiteral("FAIL  json line: ")) + gotLine);
        int extra = 1;
        const auto check = [&](const QString &name, bool ok, const QString &got) {
            ++extra;
            if (!ok) ++failed;
            print((ok ? QStringLiteral("PASS  ") : QStringLiteral("FAIL  ")) + name + QStringLiteral(": ") + got);
        };
        const auto json = [](const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); };
        const auto parse = [](const char *text) { return QJsonDocument::fromJson(QByteArray(text)).object(); };

        // geo: only the whitelisted keys, plain text, no address in any form.
        const struct { const char *in; const char *want; } geoCases[] = {
            // A city is never kept, however plain (shadowlos 2feb9c58).
            {R"({"net_type":"wifi","country":"TH","region":"Bangkok","city":"Bang Kapi","asn":45758,"org":"AIS Fibre"})",
             R"({"asn":45758,"country":"TH","net_type":"wifi","org":"AIS Fibre","region":"Bangkok"})"},
            {R"({"net_type":"ethernet","ip":"203.0.113.7","country":"RU","region":"Санкт-Петербург"})",
             R"({"country":"RU","net_type":"ethernet","region":"Санкт-Петербург"})"},
            {R"({"region":"St. Petersburg"})", R"({"region":"St. Petersburg"})"},
            {R"({"region":"2001:db8::1","org":"Amazon.com, Inc.","asn":16509})", R"({"asn":16509,"org":"AS16509"})"},
            {R"({"region":"203.0.113.7","carrier":"node1.example.de"})", R"({})"},
            {R"({"org":"see https://x.example.com","region":"aa:bb:cc:dd:ee:ff"})", R"({})"},
            {R"({"country":"th","net_type":"Wi-Fi","asn":"45758","mcc_mnc":"52003x","region":"a\tb","org":"host=x"})", R"({})"},
            {R"({"region":" padded","carrier":"Ivan.Petrov","asn":-1,"mcc_mnc":"52003"})", R"({"mcc_mnc":"52003"})"},
        };
        for (const auto &c : geoCases) {
            const QString got = json(SanitizeGeo(parse(c.in)));
            check(QStringLiteral("geo ") + QString::fromUtf8(c.in), got == QString::fromUtf8(c.want), got);
        }

        // A net line: the nested geo is checked on the way out, other nested values go.
        const QString netLine = QStringLiteral(
            R"({"ts":"2026-10-26T12:00:00.100Z","sid":4,"seq":8,"layer":"app","step":"net","outcome":"ok","d":"net_type=wifi region=203.0.113.7","geo":{"net_type":"wifi","city":"Khimki","region":"203.0.113.7","ip":"203.0.113.7","country":"TH"},"x":{"ip":"203.0.113.7"}})");
        const QString wantNet = QStringLiteral(
            R"({"ts":"2026-10-26T12:00:00.100Z","sid":4,"seq":8,"layer":"app","step":"net","outcome":"ok","d":"net_type=wifi region=<ip>","geo":{"country":"TH","net_type":"wifi"}})");
        const QString gotNet = ScrubJsonLine(netLine);
        check(QStringLiteral("net line"), gotNet == wantNet && QJsonDocument::fromJson(gotNet.toUtf8()).isObject(), gotNet);

        // The envelope: Windows shape, the latest geo, never an address.
        SetLatestGeoForTest(parse(R"({"net_type":"wifi","country":"TH","asn":45758,"org":"Amazon.com","city":"Bangkok","region":"203.0.113.7"})"));
        const QJsonObject env = UploadEnvelope(QStringLiteral("journal"), QStringLiteral("j1-2-abcdef01"), QStringLiteral("x"));
        const QString envGeo = json(env.value(QStringLiteral("geo")).toObject());
        check(QStringLiteral("envelope"),
              !env.contains(QStringLiteral("android")) && !env.contains(QStringLiteral("sdk")) &&
                  env.value(QStringLiteral("platform")).toString() == QStringLiteral("windows") &&
                  env.value(QStringLiteral("app_id")).toString() == QStringLiteral("shadowlos-throne-dev") &&
                  envGeo == QStringLiteral(R"({"asn":45758,"country":"TH","net_type":"wifi","org":"AS45758"})"),
              QStringLiteral("keys ") + QStringList(env.keys()).join(u',') + QStringLiteral(" geo ") + envGeo);
        SetLatestGeoForTest({});
        const QString keyProblems = NetworkKeySelfTest();
        check(QStringLiteral("network key"), keyProblems.isEmpty(), keyProblems.isEmpty() ? QStringLiteral("ok") : keyProblems);
        const QString censusProblems = CensusSelfTest();
        check(QStringLiteral("census"), censusProblems.isEmpty(), censusProblems.isEmpty() ? QStringLiteral("ok") : censusProblems);
        check(QStringLiteral("envelope without geo"), !UploadEnvelope(QStringLiteral("journal"), QStringLiteral("j1"), QStringLiteral("x")).contains(QStringLiteral("geo")), {});

        print(failed == 0 ? QStringLiteral("journal selftest: all %1 passed").arg(std::size(cases) + extra)
                          : QStringLiteral("journal selftest: %1 FAILED").arg(failed));
        return failed == 0 ? 0 : 1;
    }

} // namespace Shadowlos::Journal

#endif // SHADOWLOS_DEV
