#pragma once

#include <QString>

// Raw core errors (sing-box, xray, Qt network) as a short Russian sentence that says what to
// do. The raw text cannot be translated word for word, so the common causes are recognised by
// their wording and the rest fall back to a generic sentence; the original is always kept and
// shown in small text under the sentence.
namespace Shadowlos::ErrorText {

    enum class Kind {
        Unknown,
        AdminRequired,  // TUN without administrator rights
        AnotherVpn,     // another VPN holds the routes or the firewall
        TunFailed,      // the TUN adapter could not be set up
        PortInUse,      // a listen port is taken
        CacheLocked,    // cache.db held by another copy
        RuleSet,        // a remote rule-set could not be downloaded
        Dns,
        Tls,
        Timeout,
        Refused,
        Reset,
        NetworkDown,
        ServerHttp,     // the server answered with an HTTP error
        CoreDown,       // the core does not answer
        AlreadyRunning,
    };

    // [tunOn]: TUN mode is on (1) or off (0); -1 reads the settings. A plain "access is denied"
    // means missing administrator rights only for TUN.
    Kind Classify(const QString &raw, int tunOn = -1);

    // The sentence for [raw]; [fallback] when nothing is recognised (empty: none).
    QString Sentence(const QString &raw, const QString &fallback = {});

    // For a message box: the sentence, then the original in small text.
    QString Html(const QString &raw, const QString &fallback = {});

    // For one line (the log): «sentence (original)», or the original alone when unknown.
    QString Plain(const QString &raw);

    // The generic sentence of a failed connect, and the title of its message box.
    QString ConnectFallback();
    QString ConnectTitle();

    // The status card's line after a failed connect: set on failure, cleared on the next try.
    void SetConnectError(const QString &raw);
    // A line of the caller's own (a box that gives its own advice).
    void SetConnectErrorLine(const QString &line);
    void ClearConnectError();
    QString ConnectErrorLine();

    // Dev builds: a canned raw error per demo token (err-port, err-tun, ...) for screenshots.
    QString DemoRaw(const QString &token);

    // For `--journal-selftest`: empty when every case maps as expected, else what failed.
    QString SelfTest();

} // namespace Shadowlos::ErrorText
