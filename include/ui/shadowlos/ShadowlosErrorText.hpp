#pragma once

#include <QString>

class QMessageBox;

// Raw core errors (sing-box, xray, Qt network) in Russian for the user. The raw text cannot be
// translated word for word, so the common causes are recognised by their wording and the rest
// fall back to a generic sentence. Each cause has a full sentence that says what to do (the
// popup, the tooltip) and a short form of about 50 characters (the status card's line).
// Popups show the raw text small under the sentence, in a height-capped area; the log and the
// journal keep it in English as it was.
namespace Shadowlos::ErrorText {

    enum class Kind {
        Unknown,
        AdminRequired,  // TUN without administrator rights
        AnotherVpn,     // another VPN or a second ShadowLos holds the TUN adapter or the firewall
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

    // The full sentence for [raw] (popup, tooltip); [fallback] when nothing is recognised.
    QString Sentence(const QString &raw, const QString &fallback = {});

    // The short form for [raw] (status card line, about 50 characters); [fallback] when unknown.
    QString Short(const QString &raw, const QString &fallback = {});

    // Puts [raw] small under the box's text, in a scrolling area of at most about eight lines.
    // Call it last, once the box has its text and buttons.
    void AttachRaw(QMessageBox *box, const QString &raw);

    // A modal warning: [sentence], then [raw] small under it (see AttachRaw).
    int ShowError(const QString &title, const QString &sentence, const QString &raw);

    // The generic sentence of a failed connect, its short form, and the popup's title.
    QString ConnectFallback();
    QString ConnectFallbackShort();
    QString ConnectTitle();

    // The status card's line after a failed connect: set on failure, cleared on the next try.
    void SetConnectError(const QString &raw);
    // A line of the caller's own (a box that gives its own advice): the short form and the
    // full sentence for the tooltip.
    void SetConnectErrorLine(const QString &shortLine, const QString &full);
    void ClearConnectError();
    QString ConnectErrorLine();
    QString ConnectErrorTooltip();

    // Dev builds: a canned raw error per demo token (err-port, err-vpn, ...) for screenshots.
    QString DemoRaw(const QString &token);

    // For `--journal-selftest`: empty when every case maps as expected, else what failed.
    QString SelfTest();

} // namespace Shadowlos::ErrorText
