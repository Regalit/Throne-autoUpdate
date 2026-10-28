#include "include/ui/shadowlos/ShadowlosJournal.hpp"
#include "include/ui/shadowlos/ShadowlosChrome.hpp"

#if SHADOWLOS_DEV

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QLabel>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStyle>
#include <QHBoxLayout>
#include <QSet>
#include <QSslSocket>
#include <QTimer>

#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

// The blocking census of the Android app, for the dev badge. Ported from shadowlos
// app/lib/modules/probes/{network_class,census_sensor,tls_probe}.dart (itself a lift of
// shadowlos_shared/lib/shadowlos/routing/network_class_probe.dart classifyNetwork) and the
// badge text from app/lib/dev_logs/badge_geo.dart and home_screen.dart blockingNoteLabel.
// Left out: everything about the tunnel's own delivery, QUIC, the slow-link latch, the recall
// of remembered addresses, the flip chronicle: Android-only or about the tunnel, not the network.
//
// Each host gets a direct TLS handshake on :443 (never our proxy). One deadline for the
// whole pass: a finished handshake is true, a refusal/reset/TLS or name failure is false, a
// host that did not finish is left out ("not measured", never "unreachable").
namespace Shadowlos::Journal {

    namespace {

        // ── the verdict (pure) ───────────────────────────────────────────────

        enum class Blocking { None, Regular, Whitelist, NoInternet, Unknown };

        using Results = QHash<QString, bool>;

        // On the Минцифры whitelist: they survive whitelist mode, so both silent is no internet.
        const QStringList &controlHosts() {
            static const QStringList l{QStringLiteral("ya.ru"), QStringLiteral("vk.com")};
            return l;
        }
        // Typical targets of Russian blocking.
        const QStringList &foreignHosts() {
            static const QStringList l{QStringLiteral("youtube.com"), QStringLiteral("x.com"), QStringLiteral("instagram.com"),
                                       QStringLiteral("facebook.com"), QStringLiteral("tiktok.com")};
            return l;
        }
        // Usually not blocked: if these are down too, the network allows a list, not blocks one.
        const QStringList &resilientHosts() {
            static const QStringList l{QStringLiteral("wikipedia.org"), QStringLiteral("google.com"), QStringLiteral("github.com"),
                                       QStringLiteral("ifconfig.me"), QStringLiteral("api.ipify.org")};
            return l;
        }
        QStringList allHosts() {
            return controlHosts() + foreignHosts() + resilientHosts();
        }

        int answered(const Results &r, const QStringList &hosts) {
            int n = 0;
            for (const auto &h : hosts) n += r.value(h, false) ? 1 : 0;
            return n;
        }
        int probed(const Results &r, const QStringList &hosts) {
            int n = 0;
            for (const auto &h : hosts) n += r.contains(h) ? 1 : 0;
            return n;
        }

        enum class Cohort { Answers, Blocked, Inconclusive };

        // Majority, with a floor of two concluded hosts (one is a single outage).
        Cohort majority(const Results &r, const QStringList &hosts) {
            const int p = probed(r, hosts), a = answered(r, hosts);
            if (p < 2) return Cohort::Inconclusive;
            return a * 2 > p ? Cohort::Answers : Cohort::Blocked;
        }

        struct Census {
            Blocking blocking = Blocking::Unknown;
            // The network_class_probe.dart tier name, for the journal (it splits unknown in two).
            const char *tier = "controlsNotProbed";
            // One control reached, or both measured: otherwise a re-ask is worth it.
            bool controlsConcluded = false;
            Results results;

            QString summary() const {
                return QStringLiteral("контроль %1/%2, иностранные %3/%4, устойчивые %5/%6")
                    .arg(answered(results, controlHosts())).arg(probed(results, controlHosts()))
                    .arg(answered(results, foreignHosts())).arg(probed(results, foreignHosts()))
                    .arg(answered(results, resilientHosts())).arg(probed(results, resilientHosts()));
            }
            QString counts() const {
                return QStringLiteral("control=%1/%2 foreign=%3/%4 resilient=%5/%6")
                    .arg(answered(results, controlHosts())).arg(probed(results, controlHosts()))
                    .arg(answered(results, foreignHosts())).arg(probed(results, foreignHosts()))
                    .arg(answered(results, resilientHosts())).arg(probed(results, resilientHosts()));
            }
        };

        // The old app's decision tree. ORDER IS THE FIX: «never asked» before «asked and silent».
        Census classify(const Results &r) {
            Census c;
            c.results = r;
            const bool controlOk = answered(r, controlHosts()) > 0;
            const int controlProbed = probed(r, controlHosts());
            c.controlsConcluded = controlOk || controlProbed == controlHosts().size();
            if (!controlOk && controlProbed < controlHosts().size()) {
                c.blocking = Blocking::Unknown;
                c.tier = "controlsNotProbed";
                return c;
            }
            if (!controlOk) {
                c.blocking = Blocking::NoInternet;
                c.tier = "noInternet";
                return c;
            }
            const Cohort foreign = majority(r, foreignHosts());
            const Cohort resilient = majority(r, resilientHosts());
            if (foreign == Cohort::Inconclusive || resilient == Cohort::Inconclusive) {
                c.blocking = Blocking::Unknown;
                c.tier = "inconclusive";
            } else if (foreign == Cohort::Answers && resilient == Cohort::Answers) {
                c.blocking = Blocking::None;
                c.tier = "noBlocking";
            } else if (foreign == Cohort::Blocked && resilient == Cohort::Answers) {
                c.blocking = Blocking::Regular;
                c.tier = "regularBlocking";
            } else if (foreign == Cohort::Blocked && resilient == Cohort::Blocked) {
                c.blocking = Blocking::Whitelist;
                c.tier = "whitelistRegime";
            } else {
                // Foreign answers while resilient is blocked: an anomaly, no label forced.
                c.blocking = Blocking::Unknown;
                c.tier = "inconclusive";
            }
            return c;
        }

        // Second pass over the first: later evidence wins (census_sensor.dart).
        Results merged(const Results &first, const Results &second) {
            Results out = first;
            for (auto it = second.constBegin(); it != second.constEnd(); ++it) out.insert(it.key(), it.value());
            return out;
        }

        const char *word(Blocking b) {
            switch (b) {
                case Blocking::None: return "none";
                case Blocking::Regular: return "regular";
                case Blocking::Whitelist: return "whitelist";
                case Blocking::NoInternet: return "noInternet";
                default: return "unknown";
            }
        }

        bool isReal(Blocking b) {
            return b == Blocking::None || b == Blocking::Regular || b == Blocking::Whitelist;
        }

        // ── the kept verdict, per network (pure) ─────────────────────────────

        // A real verdict (none/regular/whitelist) is kept for the network it was measured on;
        // «unknown» (the probe did not answer) falls back to it; «no internet» is shown but not
        // kept: it is a passing state (Android Conductor._verdictToShow).
        class Keeper {
        public:
            struct Entry {
                std::optional<Blocking> kept;
                qint64 keptAt = 0;
                std::optional<Blocking> last; // the last measurement, whatever it was
                qint64 measuredAt = 0;
                Census census;
            };

            void record(const QByteArray &net, const Census &c, qint64 now) {
                Entry &e = byNet[net];
                if (isReal(c.blocking)) {
                    e.kept = c.blocking;
                    e.keptAt = now;
                }
                e.last = c.blocking;
                e.measuredAt = now;
                e.census = c;
            }

            // What the badge shows for [net]; nullopt before anything was measured on it.
            std::optional<Blocking> shown(const QByteArray &net) const {
                const auto it = byNet.constFind(net);
                if (it == byNet.constEnd() || !it->last) return std::nullopt;
                if (*it->last == Blocking::NoInternet) return Blocking::NoInternet;
                if (it->kept) return *it->kept;
                return Blocking::Unknown;
            }

            const Entry *entry(const QByteArray &net) const {
                const auto it = byNet.constFind(net);
                return it == byNet.constEnd() ? nullptr : &*it;
            }

        private:
            QHash<QByteArray, Entry> byNet; // memory only, like geo's cache
        };

        // ── the badge text (pure) ────────────────────────────────────────────

        // The Android dev badge's words (home_screen.dart blockingNoteLabel); unknown has none.
        QString note(std::optional<Blocking> b) {
            if (!b) return {};
            switch (*b) {
                case Blocking::None: return QStringLiteral("Нет блокировок");
                case Blocking::Regular: return QStringLiteral("Обычные блокировки");
                case Blocking::Whitelist: return QStringLiteral("Белые списки");
                case Blocking::NoInternet: return QStringLiteral("Нет интернета");
                default: return {};
            }
        }

        // «ПАО "МТС"» -> «МТС», «PJSC Mobile TeleSystems» -> «Mobile TeleSystems». At most 24
        // characters (badge_geo.dart shortOrg).
        QString shortOrg(const QString &org) {
            static const QRegularExpression legal(
                QStringLiteral(R"((?<![\p{L}\p{N}_])(?:PJSC|OJSC|JSC|CJSC|LLC|OOO|Ltd|Limited|Inc|Corp|Corporation|Co|Company|PLC|)"
                               R"(Public Joint Stock Company|ПАО|ОАО|ЗАО|ООО|АО|ИП)(?![\p{L}\p{N}_])\.?)"),
                QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
            static const QRegularExpression quotes(QStringLiteral("[«»\"“”,]"));
            static const QRegularExpression spaces(QStringLiteral(R"(\s+)"));
            QString s = QString(org).replace(legal, QStringLiteral(" ")).replace(quotes, QStringLiteral(" ")).replace(spaces, QStringLiteral(" ")).trimmed();
            if (s.size() > 24) {
                const int cut = s.lastIndexOf(u' ', 24);
                s = s.left(cut > 8 ? cut : 24) + QStringLiteral("…");
            }
            return s;
        }

        QString str(const QJsonObject &geo, const char *key) {
            return geo.value(QLatin1String(key)).toString().trimmed();
        }

        // The region, else the country.
        QString geoRegion(const QJsonObject &geo) {
            const QString r = str(geo, "region");
            return r.isEmpty() ? str(geo, "country") : r;
        }

        // The short name of the network's organisation, else the carrier on cellular.
        QString geoOperator(const QJsonObject &geo) {
            if (const QString org = str(geo, "org"); !org.isEmpty()) {
                if (const QString s = shortOrg(org); !s.isEmpty()) return s;
            }
            return str(geo, "net_type") == QStringLiteral("cellular") ? str(geo, "carrier") : QString();
        }

        // The note, the region and the operator, whatever is known, joined with « · ».
        QString badgeText(const QString &note, const QJsonObject &geo) {
            QStringList parts;
            for (const QString &p : {note, geoRegion(geo), geoOperator(geo)}) {
                if (!p.isEmpty()) parts << p;
            }
            return parts.join(QStringLiteral(" · "));
        }

        // The bottom bar's network line: region · operator · kind of network, whatever is known.
        QString geoLine(const QJsonObject &geo, const QString &netType) {
            QString kind;
            if (netType == QStringLiteral("wifi")) kind = QStringLiteral("Wi-Fi");
            else if (netType == QStringLiteral("ethernet")) kind = QStringLiteral("кабель");
            else if (netType == QStringLiteral("cellular")) kind = QStringLiteral("мобильная сеть");
            QStringList parts;
            for (const QString &p : {geoRegion(geo), geoOperator(geo), kind}) {
                if (!p.isEmpty()) parts << p;
            }
            return parts.join(QStringLiteral(" · "));
        }

        // The badge dot's colour: green none, yellow regular, red whitelist or no internet.
        const char *dotLevel(std::optional<Blocking> b) {
            if (!b) return "none";
            switch (*b) {
                case Blocking::None: return "ok";
                case Blocking::Regular: return "warn";
                case Blocking::Whitelist:
                case Blocking::NoInternet: return "bad";
                default: return "none";
            }
        }

        // ── one probe pass ───────────────────────────────────────────────────

        class TlsPass : public QObject {
        public:
            TlsPass(const QStringList &hosts, int deadlineMs, std::function<void(const Results &)> done)
                : done_(std::move(done)) {
                if (!QSslSocket::supportsSsl()) {
                    // No TLS here: nothing measured, which is not «no internet».
                    QTimer::singleShot(0, this, [this] { finish(); });
                    return;
                }
                pending_ = int(hosts.size());
                for (const QString &host : hosts) {
                    auto *socket = new QSslSocket(this);
                    socket->setProxy(QNetworkProxy::NoProxy);
                    // The default TLS configuration on purpose. Qt here has only the OpenSSL
                    // backend: no CRL or OCSP fetch. Its on-demand Windows root lookup
                    // (CertGetCertificateChain) may go to the network, but turning it off (a CA
                    // list set up front) makes a host whose root a fresh Windows store has not
                    // fetched yet fail TLS, which counts as blocking: a false «блокировки» is
                    // worse than a host now and then left unmeasured by the deadline.
                    connect(socket, &QSslSocket::encrypted, this, [this, host, socket] { conclude(host, socket, true); });
                    connect(socket, &QAbstractSocket::errorOccurred, this, [this, host, socket](QAbstractSocket::SocketError e) {
                        // A timeout is no evidence; a refusal, reset, name or TLS failure is.
                        if (e == QAbstractSocket::SocketTimeoutError) conclude(host, socket, std::nullopt);
                        else conclude(host, socket, false);
                    });
                    socket->connectToHostEncrypted(host, 443);
                }
                QTimer::singleShot(deadlineMs, this, [this] { finish(); });
            }

        private:
            void conclude(const QString &host, QSslSocket *socket, std::optional<bool> ok) {
                if (finished_ || seen_.contains(host)) return;
                seen_.insert(host);
                if (ok) results_.insert(host, *ok);
                socket->disconnect(this);
                QTimer::singleShot(0, socket, [socket] { socket->abort(); });
                if (--pending_ == 0) finish();
            }

            void finish() {
                if (finished_) return;
                finished_ = true;
                for (auto *socket : findChildren<QSslSocket *>()) {
                    socket->disconnect(this);
                    socket->abort();
                }
                const auto done = std::move(done_);
                const Results results = results_;
                deleteLater();
                done(results);
            }

            std::function<void(const Results &)> done_;
            Results results_;
            QSet<QString> seen_;
            int pending_ = 0;
            bool finished_ = false;
        };

        // ── the badge widget ─────────────────────────────────────────────────

        // As wide as its text, but never wider than its column allows: it elides instead, so it
        // never widens the window.
        class BadgeLabel : public QLabel {
        public:
            explicit BadgeLabel(QWidget *parent) : QLabel(parent) {
                setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
                setTextFormat(Qt::PlainText);
            }

            void setFullText(const QString &text) {
                if (text == full_) return;
                full_ = text;
                QLabel::setText(text); // the size hint follows the full text
                updateGeometry();
                elide();
            }

            QSize sizeHint() const override {
                ensurePolished();
                const QSize h = QLabel::sizeHint();
                // QLabel's hint is for the text shown (maybe elided): widen it to the full text.
                return {h.width() - fontMetrics().horizontalAdvance(text()) + fontMetrics().horizontalAdvance(full_) + 2, h.height()};
            }

            // Shrinks to a 48 px floor and elides: a long region line never raises the window's minimum.
            QSize minimumSizeHint() const override {
                return {qMin(sizeHint().width(), 48), sizeHint().height()};
            }

        protected:
            void resizeEvent(QResizeEvent *event) override {
                QLabel::resizeEvent(event);
                elide();
            }

        private:
            void elide() {
                const QString t = fontMetrics().elidedText(full_, Qt::ElideRight, qMax(0, contentsRect().width()));
                if (t != text()) QLabel::setText(t);
            }

            QString full_;
        };

        // ── the schedule and the badge (UI thread) ───────────────────────────

        constexpr int kTickMs = 2000;
        constexpr int kFirstStepMs = 3000;  // census_sensor.dart firstStep
        constexpr int kReaskStepMs = 7000;  // reaskStep: once, never a loop
        constexpr qint64 kRealEveryMs = 30 * 60 * 1000;
        // After unknown / no internet: 60 s, then 120 s, then every 300 s.
        constexpr qint64 kAgainSoonMs[] = {60 * 1000, 120 * 1000, 300 * 1000};

        // The wait before the next try after [notReal] measurements in a row without a real verdict.
        qint64 againSoonMs(int notReal) {
            return kAgainSoonMs[qBound(0, notReal, int(std::size(kAgainSoonMs)) - 1)];
        }
        constexpr qint64 kSettleMs = 2000;          // after a tunnel goes down
        constexpr qint64 kTeardownMaxMs = 15 * 1000;

        struct State {
            QPointer<BadgeLabel> label;
            QPointer<QLabel> dot;
            QPointer<BadgeLabel> geoLabel;
            QPointer<QWidget> geoSep;
            Keeper keeper;
            QByteArray net;
            QString netType;
            bool netKnown = false;
            bool due = true;
            QString trigger = QStringLiteral("start");
            qint64 nextAt = 0;
            bool measuring = false;
            bool tunnelWas = false;
            qint64 settleUntil = 0;
            qint64 teardownUntil = 0;
            QByteArray geoKey;       // the 15 s check's key last seen (s.net may be ahead of it)
            int netSetAtCheck = -1;  // the check counter when s.net was last set
            int seenConnects = -1;   // ConnectCount() last seen
            int notReal = 0;         // measurements in a row without a real verdict
            QString lastLogged;      // network+verdict+tier of the last census line
            std::vector<std::function<void(const CensusNow &)>> waiters; // the page's «Проверить доступность»
        };

        State &st() {
            static State s;
            return s;
        }

        qint64 nowMs() {
            return QDateTime::currentMSecsSinceEpoch();
        }

        // Whether the tick moves to [key], the network the geo check reports. [net] may be ahead
        // of the check (measure() read the adapters itself): the check catching up to it is no
        // switch, but a check taken after net was set that reports another network is one, even
        // the network the check saw before (A -> B seen only by measure() -> back to A).
        bool tickSwitches(const QByteArray &key, const QByteArray &geoKey, const QByteArray &net, int checks, int netSetAtCheck) {
            return key != net && (key != geoKey || checks != netSetAtCheck);
        }

        bool tunMode() {
            return Configs::dataManager->settingsRepo->spmode_vpn && TunnelActive();
        }

        QString tunText() {
            return QStringLiteral("Пока VPN подключён в режиме TUN, проверить нельзя: проверка идёт мимо VPN. Отключите VPN и нажмите ещё раз.");
        }

        QString fullLabel(Blocking b) {
            switch (b) {
                case Blocking::None: return QStringLiteral("блокировок нет");
                case Blocking::Regular: return QStringLiteral("обычные блокировки (youtube/instagram и т.п. недоступны, wikipedia/google доступны)");
                case Blocking::Whitelist: return QStringLiteral("белые списки (доступны только сайты класса vk.com/ya.ru)");
                case Blocking::NoInternet: return QStringLiteral("нет интернета (контроль vk.com/ya.ru не отвечает)");
                default: return QStringLiteral("не определён");
            }
        }

        QString ago(qint64 at) {
            const qint64 mins = (nowMs() - at) / 60000;
            return mins < 1 ? QStringLiteral("только что") : QStringLiteral("%1 мин назад").arg(mins);
        }

        void render() {
            auto &s = st();
            if (!s.label) return;
            const auto shown = s.keeper.shown(s.net);
            const bool tun = tunMode();
            QString n = note(shown);
            // No verdict to show yet, and one is on its way.
            if (n.isEmpty() && !tun && (s.measuring || s.due)) n = QStringLiteral("Проверяем…");
            const QJsonObject geo = LatestGeo();
            QString text = n;
            QString where = geoLine(geo, s.netType);
            const char *level = n == note(shown) ? dotLevel(shown) : "none";
            // Dev screenshots (SHADOWLOS_UI_DEMO): a made-up place, never the user's own.
            if (!Shadowlos::Chrome::UiDemo().isEmpty()) {
                text = QStringLiteral("Нет блокировок");
                where = QStringLiteral("Москва · МТС · Wi-Fi");
                level = "ok";
            }

            QStringList tip;
            tip << QStringLiteral("Тип блокировок: ") + (shown ? fullLabel(*shown) : QStringLiteral("ещё не проверяли в этой сети"));
            if (const auto *e = s.keeper.entry(s.net)) {
                tip << QStringLiteral("Проверка сайтов: ") + e->census.summary() + QStringLiteral(" (") + ago(e->measuredAt) + u')';
                if (e->kept && e->last && *e->last != *e->kept) {
                    tip << QStringLiteral("Последняя проверка: ") + fullLabel(*e->last) + QStringLiteral("; показан прошлый результат (") + ago(e->keptAt) + u')';
                }
            }
            const QString region = geoRegion(geo), op = geoOperator(geo);
            if (!region.isEmpty() || !op.isEmpty()) {
                tip << QStringLiteral("Регион: ") + (region.isEmpty() ? QStringLiteral("?") : region) +
                           QStringLiteral(", оператор: ") + (op.isEmpty() ? QStringLiteral("?") : op);
            }
            if (s.measuring) tip << QStringLiteral("Идёт проверка…");
            if (tun) tip << QStringLiteral("Пока VPN подключён в режиме TUN, проверка не идёт: показан последний результат в этой сети.");
            tip << QStringLiteral("Проверка идёт мимо VPN: vk.com/ya.ru, заблокированные и обычно доступные сайты. Только в dev-сборке.");

            s.label->setFullText(text);
            s.label->setToolTip(tip.join(u'\n'));
            s.label->setVisible(!text.isEmpty());
            if (s.dot) {
                s.dot->setVisible(!text.isEmpty());
                if (s.dot->property("slLevel").toString() != QLatin1String(level)) {
                    s.dot->setProperty("slLevel", QLatin1String(level));
                    s.dot->style()->unpolish(s.dot);
                    s.dot->style()->polish(s.dot);
                }
            }
            if (s.geoLabel) {
                s.geoLabel->setFullText(where);
                s.geoLabel->setToolTip(tip.join(u'\n'));
                s.geoLabel->setVisible(!where.isEmpty());
                if (s.geoSep) s.geoSep->setVisible(!where.isEmpty() && !text.isEmpty());
            }
        }

        void measure();

        CensusNow describe(const Census &c, qint64 at) {
            CensusNow r;
            r.measured = true;
            r.verdict = note(c.blocking);
            if (r.verdict.isEmpty()) r.verdict = QStringLiteral("Не удалось определить");
            r.counts = c.summary();
            const QJsonObject geo = LatestGeo();
            r.region = geoRegion(geo);
            r.op = geoOperator(geo);
            r.at = at;
            return r;
        }

        void answerWaiters(const CensusNow &r) {
            auto waiters = std::move(st().waiters);
            st().waiters.clear();
            for (const auto &done : waiters) done(r);
        }

        void finishMeasurement(const Census &c, int passes, const QByteArray &net, const QString &trigger, qint64 ms) {
            auto &s = st();
            s.measuring = false;
            s.keeper.record(net, c, nowMs());
            if (isReal(c.blocking)) {
                s.notReal = 0;
                s.nextAt = nowMs() + kRealEveryMs;
            } else {
                s.nextAt = nowMs() + againSoonMs(s.notReal);
                ++s.notReal;
            }
            // A line only when the verdict or tier changes (or the network does): offline, the
            // re-measures would otherwise write one every minute or so.
            const QString logged = QString::fromLatin1(net.toHex()) + u'|' + QLatin1String(word(c.blocking)) + u'|' + QLatin1String(c.tier);
            if (logged != s.lastLogged || trigger == QStringLiteral("manual")) {
                s.lastLogged = logged;
                Event("app", "census", "ok",
                      QStringLiteral("verdict=%1 tier=%2 %3 passes=%4 trigger=%5 net_type=%6")
                          .arg(QLatin1String(word(c.blocking)), QLatin1String(c.tier), c.counts())
                          .arg(passes).arg(trigger, s.netType),
                      {}, ms);
            }
            render();
            answerWaiters(describe(c, nowMs()));
        }

        // Dropped: another network now, or a connect began (TUN may have carried the probes).
        bool stale(const QByteArray &net, int epoch, const QString &trigger, qint64 ms) {
            auto &s = st();
            QString why;
            // Read now, not from the 15 s check: a switch it has not seen yet must not store
            // this verdict under the old network.
            if (net != s.net || FreshNetworkKey() != net) why = QStringLiteral("dropped: the network changed");
            else if (ConnectCount() != epoch && Configs::dataManager->settingsRepo->spmode_vpn) why = QStringLiteral("dropped: a connect started meanwhile");
            else if (tunMode()) why = QStringLiteral("dropped: the tunnel is up");
            if (why.isEmpty()) return false;
            s.measuring = false;
            s.due = true;
            Event("app", "census", "fail", QStringLiteral("trigger=%1 net_type=%2").arg(trigger, s.netType), why, ms);
            render();
            CensusNow r;
            r.note = why.contains(QStringLiteral("network")) ? QStringLiteral("Сеть сменилась во время проверки. Нажмите ещё раз.")
                     : why.contains(QStringLiteral("connect")) ? QStringLiteral("Во время проверки началось подключение. Нажмите ещё раз.")
                                                               : tunText();
            answerWaiters(r);
            return true;
        }

        void measure() {
            auto &s = st();
            // The key as the adapters say now: a switch the 15 s check has not seen yet is a new
            // network already.
            QString type;
            const QByteArray fresh = FreshNetworkKey(&type);
            if (fresh != s.net) {
                s.net = fresh;
                CurrentNetworkKey(nullptr, nullptr, &s.netSetAtCheck);
                s.netType = type;
                s.trigger = QStringLiteral("network-change");
                s.notReal = 0;
            }
            s.measuring = true;
            s.due = false;
            const QString trigger = s.trigger;
            const QByteArray net = s.net;
            const int epoch = ConnectCount();
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            render();
            new TlsPass(allHosts(), kFirstStepMs, [=](const Results &first) {
                if (stale(net, epoch, trigger, clock->elapsed())) return;
                const Census c1 = classify(first);
                if (c1.controlsConcluded && c1.blocking != Blocking::Unknown) {
                    finishMeasurement(c1, 1, net, trigger, clock->elapsed());
                    return;
                }
                // Not settled: one re-ask with the long budget. Meanwhile the badge keeps what
                // it had (the kept verdict, or «Проверяем…»); the late verdict lands below.
                new TlsPass(allHosts(), kReaskStepMs, [=](const Results &second) {
                    if (stale(net, epoch, trigger, clock->elapsed())) return;
                    finishMeasurement(classify(merged(first, second)), 2, net, trigger, clock->elapsed());
                });
            });
        }

        // A TUN session that came and went between two looks still left a tunnel to wait for.
        void noteConnects() {
            auto &s = st();
            if (const int connects = ConnectCount(); connects != s.seenConnects) {
                if (s.seenConnects >= 0 && Configs::dataManager->settingsRepo->spmode_vpn) s.tunnelWas = true;
                s.seenConnects = connects;
            }
        }

        // Whether a torn-down tunnel is still to be waited for before asking the network
        // anything (it arms the wait when a tunnel went down since the last look). Not in TUN mode.
        bool teardownWaiting(qint64 now) {
            auto &s = st();
            if (s.tunnelWas) {
                s.tunnelWas = false;
                s.settleUntil = now + kSettleMs;
                s.teardownUntil = now + kTeardownMaxMs;
            }
            return now < s.settleUntil || (now < s.teardownUntil && TunAdapterUp());
        }

        void tick() {
            auto &s = st();
            QString type;
            bool checked = false;
            int checks = 0;
            const QByteArray key = CurrentNetworkKey(&type, &checked, &checks);
            if (!checked) return; // geo's first look has not run yet
            if (!s.netKnown || tickSwitches(key, s.geoKey, s.net, checks, s.netSetAtCheck)) {
                // A verdict belongs to the network it was measured on: a new one is measured anew
                // (one seen before shows its own kept verdict meanwhile).
                s.trigger = s.netKnown ? QStringLiteral("network-change") : QStringLiteral("start");
                s.net = key;
                s.netSetAtCheck = checks;
                s.netType = type;
                s.due = true;
                s.notReal = 0;
            }
            s.geoKey = key;
            s.netKnown = true;
            const qint64 now = nowMs();
            noteConnects();
            if (tunMode()) {
                // Our tunnel takes every socket: no probe, the last verdict stays.
                s.tunnelWas = true;
                render();
                if (!s.waiters.empty()) {
                    CensusNow r;
                    r.note = tunText();
                    answerWaiters(r);
                }
                return;
            }
            if (!s.due && !s.measuring && now >= s.nextAt) {
                s.due = true;
                s.trigger = QStringLiteral("refresh");
            }
            if (s.due && !s.measuring && !teardownWaiting(now)) measure();
            render();
        }

    } // namespace

    QWidget *MakeBlockingBadge(QWidget *parent) {
        // For the bottom bar: a dot and the verdict, then region · operator · network. A row of
        // fixed height that is there with or without them, so the bar does not jump.
        auto *row = new QWidget(parent);
        row->setObjectName(QStringLiteral("slBlockingBadgeRow"));
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *dot = new QLabel(row);
        dot->setObjectName(QStringLiteral("slBlockingDot"));
        dot->setFixedSize(8, 8);
        dot->setVisible(false);
        layout->addWidget(dot);
        auto *label = new BadgeLabel(row);
        label->setObjectName(QStringLiteral("slBlockingBadge"));
        label->setProperty("slRole", "badge");
        label->setFullText(QStringLiteral("x"));
        row->setFixedHeight(label->sizeHint().height());
        label->setFullText({});
        label->setVisible(false);
        layout->addWidget(label);
        auto *sep = new QFrame(row);
        sep->setObjectName(QStringLiteral("slSep"));
        sep->setFixedSize(1, 14);
        sep->setVisible(false);
        layout->addSpacing(4);
        layout->addWidget(sep);
        layout->addSpacing(4);
        auto *where = new BadgeLabel(row);
        where->setObjectName(QStringLiteral("slGeoLine"));
        where->setProperty("slRole", "badge");
        where->setVisible(false);
        layout->addWidget(where);
        row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        st().dot = dot;
        st().geoLabel = where;
        st().geoSep = sep;
        st().label = label;
        st().seenConnects = ConnectCount();
        auto *timer = new QTimer(label);
        timer->setInterval(kTickMs);
        QObject::connect(timer, &QTimer::timeout, label, [] { tick(); });
        timer->start();
        QTimer::singleShot(500, label, [] { tick(); });
        return row;
    }

    bool CensusMeasureNow(std::function<void(const CensusNow &)> done) {
        auto &s = st();
        noteConnects();
        if (tunMode()) {
            CensusNow r;
            r.note = tunText();
            done(r);
            return false;
        }
        s.waiters.push_back(std::move(done));
        if (s.measuring) return false;
        s.trigger = QStringLiteral("manual");
        // The tick's own test: a tunnel just torn down may still carry the probes. The tick
        // measures once it is gone (at most kTeardownMaxMs).
        if (teardownWaiting(nowMs())) {
            s.due = true;
            render();
            return true;
        }
        measure();
        return false;
    }

    bool CensusLast(CensusNow *out) {
        const auto *e = st().keeper.entry(st().net);
        if (e == nullptr || !e->last) return false;
        *out = describe(e->census, e->measuredAt);
        return true;
    }

    QString CensusSelfTest() {
        QStringList bad;
        const auto all = [](const QStringList &hosts, bool v) {
            Results r;
            for (const auto &h : hosts) r.insert(h, v);
            return r;
        };
        const auto with = [](Results a, const Results &b) { return merged(a, b); };
        const auto expect = [&bad](const char *name, const Census &c, Blocking b, const char *tier) {
            if (c.blocking != b || qstrcmp(c.tier, tier) != 0) {
                bad << QStringLiteral("%1: got %2/%3").arg(QLatin1String(name), QLatin1String(word(c.blocking)), QLatin1String(c.tier));
            }
        };
        const Results controlsOk = all(controlHosts(), true);
        // net_tier_from_absence_test.dart
        expect("empty pass", classify({}), Blocking::Unknown, "controlsNotProbed");
        if (classify({}).controlsConcluded) bad << QStringLiteral("empty pass concluded the controls");
        expect("controls omitted, cohorts answer", classify(with(all(foreignHosts(), true), all(resilientHosts(), true))), Blocking::Unknown, "controlsNotProbed");
        expect("controls silent", classify(all(controlHosts(), false)), Blocking::NoInternet, "noInternet");
        if (!classify(all(controlHosts(), false)).controlsConcluded) bad << QStringLiteral("both silent controls did not conclude");
        expect("one refusal, partner never asked", classify(Results{{QStringLiteral("ya.ru"), false}}), Blocking::Unknown, "controlsNotProbed");
        Results refusedAnswered = with(all(foreignHosts(), false), all(resilientHosts(), true));
        refusedAnswered.insert(QStringLiteral("ya.ru"), false);
        refusedAnswered.insert(QStringLiteral("vk.com"), true);
        expect("a refusal whose partner answered", classify(refusedAnswered), Blocking::Regular, "regularBlocking");
        // The four regimes.
        expect("open", classify(with(controlsOk, with(all(foreignHosts(), true), all(resilientHosts(), true)))), Blocking::None, "noBlocking");
        expect("regular", classify(with(controlsOk, with(all(foreignHosts(), false), all(resilientHosts(), true)))), Blocking::Regular, "regularBlocking");
        expect("whitelist", classify(with(controlsOk, with(all(foreignHosts(), false), all(resilientHosts(), false)))), Blocking::Whitelist, "whitelistRegime");
        expect("foreign up, resilient down", classify(with(controlsOk, with(all(foreignHosts(), true), all(resilientHosts(), false)))), Blocking::Unknown, "inconclusive");
        // Majority: one concluded host is no cohort verdict; half is not a majority.
        Results oneForeign = with(controlsOk, all(resilientHosts(), true));
        oneForeign.insert(QStringLiteral("youtube.com"), false);
        expect("one foreign host concluded", classify(oneForeign), Blocking::Unknown, "inconclusive");
        Results half = with(controlsOk, all(resilientHosts(), true));
        half.insert(QStringLiteral("youtube.com"), true);
        half.insert(QStringLiteral("x.com"), true);
        half.insert(QStringLiteral("instagram.com"), false);
        half.insert(QStringLiteral("facebook.com"), false);
        expect("foreign 2/4", classify(half), Blocking::Regular, "regularBlocking");
        half.insert(QStringLiteral("tiktok.com"), true);
        expect("foreign 3/5", classify(half), Blocking::None, "noBlocking");
        // The re-ask lands over the first pass: later evidence wins, earlier answers stay.
        const Results first = all(resilientHosts(), true);
        const Results second = with(controlsOk, all(foreignHosts(), false));
        expect("re-ask merged", classify(merged(first, second)), Blocking::Regular, "regularBlocking");
        expect("re-ask overrides", classify(merged(with(controlsOk, all(foreignHosts(), true)), with(all(foreignHosts(), false), all(resilientHosts(), true)))),
               Blocking::Regular, "regularBlocking");

        // The kept verdict belongs to its network.
        Keeper k;
        const QByteArray a = "net-a", b = "net-b";
        const auto shown = [&k](const QByteArray &n) { return k.shown(n); };
        Census regular = classify(with(controlsOk, with(all(foreignHosts(), false), all(resilientHosts(), true))));
        if (shown(a)) bad << QStringLiteral("a verdict before any measurement");
        k.record(a, regular, 1);
        if (shown(a) != Blocking::Regular) bad << QStringLiteral("a real verdict was not shown");
        k.record(a, classify({}), 2);
        if (shown(a) != Blocking::Regular) bad << QStringLiteral("unknown did not fall back to the kept verdict");
        k.record(a, classify(all(controlHosts(), false)), 3);
        if (shown(a) != Blocking::NoInternet) bad << QStringLiteral("no internet was not shown");
        k.record(a, classify({}), 4);
        if (shown(a) != Blocking::Regular) bad << QStringLiteral("no internet replaced the kept verdict");
        if (shown(b)) bad << QStringLiteral("another network showed this network's verdict");
        k.record(b, classify({}), 5);
        if (shown(b) != Blocking::Unknown) bad << QStringLiteral("unknown on a new network was not unknown");
        if (shown(a) != Blocking::Regular) bad << QStringLiteral("back on network a lost its verdict");

        // Switches seen by the tick, with measure() reading the adapters ahead of the 15 s check.
        {
            const QByteArray A = "net-a", B = "net-b", C = "net-c";
            if (tickSwitches(A, A, A, 7, 7)) bad << QStringLiteral("a steady network was a switch");
            if (!tickSwitches(C, A, A, 8, 7)) bad << QStringLiteral("a switch the check saw was missed");
            // measure() moved to B at check 5; the check has not looked again and still says A.
            if (tickSwitches(A, A, B, 5, 5)) bad << QStringLiteral("the check's stale key undid measure()'s fresher one");
            // The check catches up to B: no second measurement.
            if (tickSwitches(B, A, B, 6, 5)) bad << QStringLiteral("the check catching up was a switch");
            // Back on A before the check ever saw B: the next check (6) still says A.
            if (!tickSwitches(A, A, B, 6, 5)) bad << QStringLiteral("a quick switch back was missed");
        }

        // Offline back-off: 60 s, 120 s, then 300 s for good.
        if (againSoonMs(0) != 60000 || againSoonMs(1) != 120000 || againSoonMs(2) != 300000 || againSoonMs(9) != 300000) {
            bad << QStringLiteral("back-off is not 60/120/300 s");
        }

        // home_geo_badge_test.dart
        const auto eq = [&bad](const char *name, const QString &got, const QString &want) {
            if (got != want) bad << QStringLiteral("%1: got «%2» want «%3»").arg(QLatin1String(name), got, want);
        };
        eq("shortOrg ПАО", shortOrg(QStringLiteral("ПАО «МТС»")), QStringLiteral("МТС"));
        eq("shortOrg PJSC", shortOrg(QStringLiteral("PJSC Mobile TeleSystems")), QStringLiteral("Mobile TeleSystems"));
        eq("shortOrg OOO Ltd", shortOrg(QStringLiteral("OOO \"Ростелеком\", Ltd.")), QStringLiteral("Ростелеком"));
        eq("shortOrg only a form", shortOrg(QStringLiteral("ПАО")), QString());
        const QString longOrg = shortOrg(QStringLiteral("Some Very Long Organisation Name Of A Provider"));
        if (!longOrg.endsWith(QStringLiteral("…")) || longOrg.size() > 25) bad << QStringLiteral("shortOrg long: ") + longOrg;
        const QJsonObject geo{{"region", QStringLiteral("Краснодарский край")}, {"org", QStringLiteral("ПАО МТС")}, {"country", "RU"}};
        eq("badge full", badgeText(note(Blocking::Whitelist), geo), QStringLiteral("Белые списки · Краснодарский край · МТС"));
        eq("badge no note", badgeText(note(Blocking::Unknown), geo), QStringLiteral("Краснодарский край · МТС"));
        eq("badge country", badgeText(note(Blocking::Whitelist), QJsonObject{{"country", "RU"}}), QStringLiteral("Белые списки · RU"));
        eq("badge note only", badgeText(note(Blocking::Regular), {}), QStringLiteral("Обычные блокировки"));
        eq("badge nothing", badgeText(note(std::nullopt), {}), QString());
        eq("badge none", note(Blocking::None), QStringLiteral("Нет блокировок"));
        eq("badge no internet", note(Blocking::NoInternet), QStringLiteral("Нет интернета"));
        eq("operator wifi carrier", geoOperator(QJsonObject{{"carrier", "Beeline"}, {"net_type", "wifi"}}), QString());
        return bad.join(QStringLiteral("; "));
    }

} // namespace Shadowlos::Journal

#endif // SHADOWLOS_DEV
