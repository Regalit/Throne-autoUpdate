#include "include/ui/shadowlos/ShadowlosJournal.hpp"

#if SHADOWLOS_DEV

#include "include/database/DatabaseManager.h"
#include "include/database/SettingsRepo.h"
#include "include/global/Logger.hpp"
#include "include/global/ShadowlosBootstrap.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <cstring>
#include <functional>

namespace Shadowlos::Journal {

    namespace {

        constexpr qint64 kMaxFileBytes = 2 * 1024 * 1024;
        constexpr int kMaxFieldChars = 4000;
        constexpr qint64 kInfoWindowMs = 30 * 1000;
        constexpr int kCoreLinesPerMinute = 120;
        constexpr int kFirstUploadMs = 20 * 1000;
        constexpr int kUploadEveryMs = 3 * 60 * 1000;
        constexpr int kNextPacketMs = 20 * 1000; // the server takes 4 uploads a minute per user
        constexpr int kPacketChars = 90000;
        constexpr int kMinPacketChars = 4000;
        constexpr int kCrashJournalLines = 150;
        constexpr int kCrashSummaryChars = 60000;
        constexpr int kCrashReportChars = 90000;
        constexpr int kCrashReportsKept = 3;
        constexpr qint64 kHoldSecs = 30 * 60;
        constexpr qint64 kBackoffMins[] = {6, 12, 24, 30};
        constexpr auto kUploadPath = "/api/mobile/client-logs";
        constexpr auto kCrashSeparator = "--- journal of the previous run (last 150 lines) ---";
        constexpr int kManualReportChars = 60000;

        QString translate(const char *text) {
            return QCoreApplication::translate("ShadowlosJournal", text);
        }

        Configs::SettingsRepo *settings() {
            return Configs::dataManager->settingsRepo.get();
        }

        bool consentOn() {
            return settings()->shadowlos_send_journal != 0;
        }

        // The journal is written from the UI thread, the log thread and request threads,
        // so everything here is behind the one mutex.
        struct State {
            QMutex mutex;
            QString dir;
            QFile file;
            bool on = false; // started, consented and open
            qint64 sid = 0;
            qint64 seq = 0;
            QElapsedTimer connectClock; // since the last connect start
            bool connecting = false;
            int connects = 0; // ConnectStart calls this run (geo drops a whereami answer that spans one)
            QString lastFail; // the last error line since the connect started
            QElapsedTimer upClock;
            QElapsedTimer coreMinute;
            int coreLines = 0;
            int coreSkipped = 0;
        };

        State &st() {
            static State state;
            return state;
        }

        // Set once in Start(), before anything else runs.
        QString path(const char *name) {
            return st().dir + u'/' + QLatin1String(name);
        }

        QString quoted(const QString &s) {
            QString out;
            out.reserve(s.size() + 2);
            out += u'"';
            for (const QChar c : s) {
                switch (c.unicode()) {
                    case '"': out += QStringLiteral("\\\""); break;
                    case '\\': out += QStringLiteral("\\\\"); break;
                    case '\n': out += QStringLiteral("\\n"); break;
                    case '\r': out += QStringLiteral("\\r"); break;
                    case '\t': out += QStringLiteral("\\t"); break;
                    default:
                        if (c.unicode() < 0x20) out += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
                        else out += c;
                }
            }
            return out + u'"';
        }

        QString nowTs() {
            return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        }

        qint64 seqOf(const QString &line) {
            static const QRegularExpression re(QStringLiteral(R"("seq":(\d+))"));
            const auto m = re.match(line);
            return m.hasMatch() ? m.captured(1).toLongLong() : -1;
        }

        QStringList readLinesLocked() {
            if (st().file.isOpen()) st().file.flush();
            QFile f(path("journal.jsonl"));
            if (!f.open(QIODevice::ReadOnly)) return {};
            return QString::fromUtf8(f.readAll()).split(u'\n', Qt::SkipEmptyParts);
        }

        void saveStateLocked() {
            QSaveFile f(path("state.json"));
            if (!f.open(QIODevice::WriteOnly)) return;
            f.write(QJsonDocument(QJsonObject{{"sid", st().sid}, {"seq", st().seq}}).toJson(QJsonDocument::Compact));
            f.commit();
        }

        // Over 2 MiB: the older half goes, and a gap line (no seq) says which lines.
        void trimLocked() {
            auto &s = st();
            const QStringList lines = readLinesLocked();
            s.file.close();
            const int cut = int(lines.size() / 2);
            qint64 first = -1, last = -1, count = 0;
            for (int i = 0; i < cut; ++i) {
                const qint64 seq = seqOf(lines[i]);
                if (seq < 0) continue;
                if (first < 0) first = seq;
                last = seq;
                ++count;
            }
            QSaveFile out(path("journal.jsonl"));
            if (out.open(QIODevice::WriteOnly)) {
                const QString gap = QStringLiteral("{\"ts\":") + quoted(nowTs()) +
                                    QStringLiteral(",\"layer\":\"app\",\"step\":\"gap\",\"outcome\":\"fail\",\"err\":") +
                                    quoted(QStringLiteral("journal lines %1..%2 (%3) were dropped from the file (2 MiB cap)")
                                               .arg(first).arg(last).arg(count)) +
                                    u'}';
                out.write(gap.toUtf8() + '\n');
                for (int i = cut; i < lines.size(); ++i) out.write(lines[i].toUtf8() + '\n');
                out.commit();
            }
            s.on = s.file.open(QIODevice::Append);
        }

        // layer, step and outcome are our own fixed words; d and err are cut and scrubbed.
        // geo (the net line's nested object) goes in sanitized, after d.
        void writeLocked(const char *layer, const char *step, const char *outcome,
                         const QString &d = {}, const QString &err = {}, qint64 ms = -1,
                         const QJsonObject *geo = nullptr) {
            auto &s = st();
            if (!s.on) return;
            QString line = QStringLiteral("{\"ts\":") + quoted(nowTs()) +
                           QStringLiteral(",\"sid\":%1,\"seq\":%2,\"layer\":\"%3\",\"step\":\"%4\",\"outcome\":\"%5\"")
                               .arg(s.sid).arg(++s.seq).arg(QLatin1String(layer), QLatin1String(step), QLatin1String(outcome));
            if (ms >= 0) line += QStringLiteral(",\"ms\":%1").arg(ms);
            if (!err.isEmpty()) line += QStringLiteral(",\"err\":") + quoted(Scrub(err.left(kMaxFieldChars)));
            if (!d.isEmpty()) line += QStringLiteral(",\"d\":") + quoted(Scrub(d.left(kMaxFieldChars)));
            if (geo != nullptr) {
                line += QStringLiteral(",\"geo\":") +
                        QString::fromUtf8(QJsonDocument(SanitizeGeo(*geo)).toJson(QJsonDocument::Compact));
            }
            line += QStringLiteral("}\n");
            s.file.write(line.toUtf8());
            s.file.flush();
            if (s.file.size() > kMaxFileBytes) trimLocked();
        }

        QString osName() {
            return (QSysInfo::prettyProductName() + QStringLiteral(" (") + QSysInfo::kernelVersion() + u')').left(64);
        }

        // Make and product from the BIOS, e.g. "LENOVO 20XW0026RT"; never the host name.
        QString pcModel() {
            QString model;
#ifdef Q_OS_WIN
            QSettings bios(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\BIOS"), QSettings::NativeFormat);
            model = (bios.value(QStringLiteral("SystemManufacturer")).toString() + u' ' +
                     bios.value(QStringLiteral("SystemProductName")).toString()).simplified();
#endif
            model = Scrub(model).left(64);
            return model.isEmpty() ? QStringLiteral("PC") : model;
        }

        // A previous run that missed Logging::Shutdown: the crash report Throne wrote then
        // (crashes/*.txt, newer than that run's start), or a note that there is none, plus the
        // run's last journal lines. Kept as crash-<request id>.txt until the server takes it.
        QString queueCrashReportLocked(const QStringList &lines, qint64 previousSid, const QDateTime &previousStart) {
            QString summary;
            const auto reports = QDir(Logging::CrashDir()).entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Time);
            if (!reports.isEmpty() && (!previousStart.isValid() || reports.first().lastModified() >= previousStart)) {
                QFile f(reports.first().absoluteFilePath());
                if (f.open(QIODevice::ReadOnly)) summary = QString::fromUtf8(f.readAll()).right(kCrashSummaryChars);
            }
            if (summary.isEmpty()) {
                summary = QStringLiteral("The previous run did not shut down cleanly and left no crash report "
                                         "(killed, power loss, or a fault the crash handler missed).");
            }
            const QString sidKey = QStringLiteral("\"sid\":%1,").arg(previousSid);
            QStringList tail;
            for (const auto &line : lines) {
                if (line.contains(sidKey)) tail << ScrubJsonLine(line);
            }
            tail = tail.mid(qMax<qsizetype>(0, tail.size() - kCrashJournalLines));
            summary = Scrub(summary);
            // One report stays under the packet size: the oldest journal lines go first.
            qsizetype size = summary.size() + 80;
            for (const auto &line : tail) size += line.size() + 1;
            while (size > kCrashReportChars && !tail.isEmpty()) size -= tail.takeFirst().size() + 1;

            const QString id = QString::fromLatin1(QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0') +
                                                   QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'));
            QSaveFile out(st().dir + QStringLiteral("/crash-") + id + QStringLiteral(".txt"));
            if (out.open(QIODevice::WriteOnly)) {
                out.write((summary +u'\n' + QLatin1String(kCrashSeparator) + u'\n' + tail.join(u'\n')).toUtf8());
                out.commit();
            }
            // Only the newest few reports wait for the server.
            const auto queued = QDir(st().dir).entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time);
            for (qsizetype i = kCrashReportsKept; i < queued.size(); ++i) QFile::remove(queued[i].absoluteFilePath());
            static const QRegularExpression cause(QStringLiteral(R"(exception\s*:\s*(.+))"));
            const auto m = cause.match(summary);
            return m.hasMatch() ? m.captured(1).trimmed() : QStringLiteral("unclean exit, no crash report");
        }

        // Oldest first.
        QStringList crashFiles() {
            return QDir(st().dir).entryList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time | QDir::Reversed);
        }

        // ── upload ─────────────────────────────────────────────────────────────

        // UI thread only.
        struct Upload {
            QPointer<QNetworkAccessManager> nam;
            QPointer<QTimer> timer;
            bool busy = false;
            qint64 holdUntil = 0; // seconds since epoch
            int fails = 0;
            int chunk = kPacketChars;
        };

        Upload &up() {
            static Upload upload;
            return upload;
        }

        void tick();

        void scheduleSoon() {
            if (up().timer) QTimer::singleShot(kNextPacketMs, up().timer, [] { tick(); });
        }

        qint64 loadSent() {
            QFile f(path("upload.json"));
            if (!f.open(QIODevice::ReadOnly)) return 0;
            return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("sent")).toInteger();
        }

        void saveSent(qint64 sent) {
            QSaveFile f(path("upload.json"));
            if (!f.open(QIODevice::WriteOnly)) return;
            f.write(QJsonDocument(QJsonObject{{"sent", sent}}).toJson(QJsonDocument::Compact));
            f.commit();
        }

        // Dropped writes an upload-dropped line; a 400 (a bad packet) is dropped quietly.
        enum class Verdict { Accepted, Dropped, DroppedQuietly, Retry };

        // 200 (a "duplicate" too) is taken; 429/503 hold until Retry-After; 413 halves the
        // packet and drops it once the size is at the minimum (a line that will never fit);
        // 400 is dropped quietly, any other 4xx but 401 with a line; the rest backs off
        // 6, 12, 24, then 30 min.
        Verdict judge(int status, qint64 retryAfter) {
            auto &u = up();
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (status == 200) {
                u.fails = 0;
                return Verdict::Accepted;
            }
            if (status == 429 || status == 503) {
                u.holdUntil = now + (retryAfter > 0 ? retryAfter : kHoldSecs);
                return Verdict::Retry;
            }
            if (status == 413) {
                if (u.chunk <= kMinPacketChars) return Verdict::Dropped;
                u.chunk = qMax(u.chunk / 2, kMinPacketChars);
                scheduleSoon();
                return Verdict::Retry;
            }
            if (status == 400) return Verdict::DroppedQuietly;
            if (status >= 400 && status < 500 && status != 401) return Verdict::Dropped;
            u.holdUntil = now + kBackoffMins[qMin(u.fails++, 3)] * 60;
            return Verdict::Retry;
        }

        // Straight to our server, never through our own inbound or the system proxy. A manual
        // report ([queued] false) goes out beside the queue and leaves its busy flag alone.
        void post(const QString &reason, const QString &requestId, const QString &log,
                  const std::function<void(int status, qint64 retryAfter)> &done, bool queued = true) {
            QString token;
            if (!up().nam || !ApiToken(&token)) return;
            const QJsonObject body = UploadEnvelope(reason, requestId, log);
            QNetworkRequest request(QUrl(QLatin1String(ApiBase) + QLatin1String(kUploadPath)));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setRawHeader("Authorization", "Sl-Sub " + token.toUtf8());
            request.setRawHeader("X-Sl-Install-Id", InstallId().toUtf8());
            request.setRawHeader("X-Sl-Platform", "windows");
            request.setTransferTimeout(30 * 1000);
            QElapsedTimer clock;
            clock.start();
            if (queued) up().busy = true;
            auto *reply = up().nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
            QObject::connect(reply, &QNetworkReply::finished, up().nam, [reply, clock, done, queued] {
                reply->deleteLater();
                if (queued) up().busy = false;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                // Only failures are journaled: a line per success would itself be sent every round.
                if (status != 200) {
                    Event("api", "call", "fail",
                          QStringLiteral("POST %1 %2").arg(QLatin1String(kUploadPath), status ? QString::number(status) : QStringLiteral("error")),
                          status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString(), clock.elapsed());
                    LOG_INFO(QStringLiteral("[journal] upload answered %1").arg(status ? QString::number(status) : reply->errorString()));
                }
                done(status, reply->rawHeader("Retry-After").trimmed().toLongLong());
            });
        }

        void sendCrash(const QString &file) {
            QFile f(st().dir + u'/' + file);
            if (!f.open(QIODevice::ReadOnly)) return;
            const QString text = QString::fromUtf8(f.readAll());
            f.close();
            // Scrubbed again, each part its own way: the journal lines as JSON.
            const qsizetype at = text.indexOf(QLatin1String(kCrashSeparator));
            QString log = Scrub(at < 0 ? text : text.left(at));
            if (at >= 0) {
                log += QLatin1String(kCrashSeparator);
                for (const auto &line : text.mid(at + qsizetype(strlen(kCrashSeparator))).split(u'\n', Qt::SkipEmptyParts))
                    log += u'\n' + ScrubJsonLine(line);
            }
            const QString id = file.mid(6, file.size() - 10); // crash-<id>.txt
            post(QStringLiteral("crash"), id, log, [file](int status, qint64 retryAfter) {
                // A crash report is not split, so 413 drops it too.
                const Verdict verdict = status == 413 ? Verdict::Dropped : judge(status, retryAfter);
                if (verdict == Verdict::Retry) return;
                QFile::remove(st().dir + u'/' + file);
                if (verdict == Verdict::Dropped) Event("app", "upload-dropped", "fail", {}, QStringLiteral("crash report refused: HTTP %1").arg(status));
                scheduleSoon();
            });
        }

        // The next packet: whole lines after the last sent seq, up to the chunk size. Gap lines
        // (no seq) ride along when they come after what was sent.
        void sendJournal() {
            auto &u = up();
            const qint64 sent = loadSent();
            QStringList lines;
            {
                QMutexLocker lock(&st().mutex);
                lines = readLinesLocked();
            }
            qint64 firstInFile = -1;
            for (const auto &line : lines) {
                if ((firstInFile = seqOf(line)) >= 0) break;
            }
            QStringList packet;
            qint64 first = -1, last = -1, chars = 0, lastSeen = -1;
            for (const auto &line : lines) {
                const qint64 seq = seqOf(line);
                if (seq < 0) {
                    const bool fresh = lastSeen < 0 ? firstInFile - 1 > sent : lastSeen >= sent;
                    if (!fresh) continue;
                } else {
                    lastSeen = seq;
                    if (seq <= sent) continue;
                }
                if (!packet.isEmpty() && chars + line.size() + 1 > u.chunk) break;
                packet << ScrubJsonLine(line);
                chars += line.size() + 1;
                if (seq >= 0) {
                    if (first < 0) first = seq;
                    last = seq;
                }
            }
            if (first < 0) return;
            const QString id = QStringLiteral("j%1-%2-%3").arg(first).arg(last).arg(InstallId().remove(u'-').left(8));
            post(QStringLiteral("journal"), id, packet.join(u'\n'), [first, last](int status, qint64 retryAfter) {
                const Verdict verdict = judge(status, retryAfter);
                if (verdict == Verdict::Retry) return;
                saveSent(last);
                up().chunk = kPacketChars; // taken or dropped: the next packet starts full size again
                if (verdict == Verdict::Dropped) {
                    Event("app", "upload-dropped", "fail", {},
                          QStringLiteral("journal lines %1..%2 refused: HTTP %3").arg(first).arg(last).arg(status));
                }
                QMutexLocker lock(&st().mutex);
                if (st().seq > last) scheduleSoon();
            });
        }

        // Held while sending is off, a request is out, the server asked to wait, or a
        // connect is being brought up. A pending crash report goes first.
        void tick() {
            auto &u = up();
            if (u.busy || !consentOn() || QDateTime::currentSecsSinceEpoch() < u.holdUntil) return;
            {
                QMutexLocker lock(&st().mutex);
                if (st().connecting) return;
            }
            QString token;
            if (!ApiToken(&token)) return;
            if (const QStringList crashes = crashFiles(); !crashes.isEmpty()) {
                sendCrash(crashes.first());
                return;
            }
            sendJournal();
        }

        // Off clears the journal, the queue and any crash report; seq keeps counting.
        void setConsent(bool on) {
            settings()->shadowlos_send_journal = on ? 1 : 0;
            settings()->Save();
            auto &s = st();
            QMutexLocker lock(&s.mutex);
            if (!on) {
                s.on = false;
                s.file.close();
                QFile::remove(path("journal.jsonl"));
                QFile::remove(path("upload.json"));
                for (const auto &file : crashFiles()) QFile::remove(s.dir + u'/' + file);
                saveStateLocked();
                up().holdUntil = 0;
                up().fails = 0;
                LOG_INFO(QStringLiteral("[journal] sending turned off; journal, queue and crash report cleared"));
                return;
            }
            s.file.setFileName(path("journal.jsonl"));
            s.on = s.file.open(QIODevice::Append);
            writeLocked("app", "settings", "ok", QStringLiteral("journal on"));
            LOG_INFO(QStringLiteral("[journal] sending turned on"));
        }

        QPointer<QAction> consentAction;

        // ── ui ─────────────────────────────────────────────────────────────────

        QString label(QString text, const QString &name) {
            text = text.remove(u'&').simplified();
            if (text.isEmpty()) text = name;
            if (text.isEmpty()) return QStringLiteral("(no label)");
            return text.contains(u'@') ? QStringLiteral("(hidden)") : text;
        }

        // Dialogs opening and closing, warning boxes, taps on buttons and menu items; an
        // application-wide event filter, so no upstream dialog needs a hook.
        class UiTracker : public QObject {
        public:
            using QObject::QObject;

        protected:
            bool eventFilter(QObject *obj, QEvent *event) override {
                switch (event->type()) {
                    case QEvent::Show:
                        if (auto *dialog = qobject_cast<QDialog *>(obj)) opened(dialog);
                        break;
                    case QEvent::Hide:
                        if (auto *dialog = qobject_cast<QDialog *>(obj)) closed(dialog);
                        break;
                    case QEvent::MouseButtonRelease:
                        released(obj, static_cast<QMouseEvent *>(event));
                        break;
                    default:
                        break;
                }
                return false;
            }

        private:
            QHash<QObject *, QElapsedTimer> open;

            void opened(QDialog *dialog) {
                if (open.contains(dialog)) return;
                open[dialog].start();
                connect(dialog, &QObject::destroyed, this, [this, dialog] { open.remove(dialog); });
                Event("ui", "screen-open", "ok", QString::fromLatin1(dialog->metaObject()->className()));
                if (auto *box = qobject_cast<QMessageBox *>(dialog)) {
                    // TUN needs admin: the "run as admin" question is a normal step, not a failure.
                    if (box->text() == QCoreApplication::translate("MainWindow", "Please run Throne as admin"))
                        Event("app", "elevate", "ok", QStringLiteral("asked to restart as admin"));
                    else if (box->icon() == QMessageBox::Warning || box->icon() == QMessageBox::Critical)
                        Event("app", "error", "fail", box->windowTitle(), box->text());
                }
            }

            void closed(QDialog *dialog) {
                const auto it = open.find(dialog);
                if (it == open.end()) return;
                Event("ui", "screen-close", "ok", QString::fromLatin1(dialog->metaObject()->className()), {}, it->elapsed());
                open.erase(it);
            }

            static void released(QObject *obj, QMouseEvent *event) {
                if (event->button() != Qt::LeftButton) return;
                const QPoint at = event->position().toPoint();
                if (auto *button = qobject_cast<QAbstractButton *>(obj)) {
                    if (button->isEnabled() && button->rect().contains(at))
                        Event("ui", "tap", "ok", label(button->text(), button->objectName()));
                } else if (auto *menu = qobject_cast<QMenu *>(obj)) {
                    if (auto *action = menu->actionAt(at); action && !action->isSeparator() && action->isEnabled())
                        Event("ui", "tap", "ok", label(action->text(), action->objectName()));
                }
            }
        };

    } // namespace

    void Start() {
        auto &s = st();
        QMutexLocker lock(&s.mutex);
        if (!s.dir.isEmpty()) return;
        s.dir = QDir::current().absoluteFilePath(QStringLiteral("journal"));
        QDir().mkpath(s.dir);

        // sid counts launches; seq goes on from the larger of the saved one and the file's
        // last line (a crash skips saving it).
        QFile state(path("state.json"));
        const QDateTime previousStart = QFileInfo(state).lastModified();
        if (state.open(QIODevice::ReadOnly)) {
            const auto obj = QJsonDocument::fromJson(state.readAll()).object();
            s.sid = obj.value(QStringLiteral("sid")).toInteger();
            s.seq = obj.value(QStringLiteral("seq")).toInteger();
            state.close();
        }
        const QStringList lines = readLinesLocked();
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
            if (const qint64 seq = seqOf(*it); seq >= 0) {
                s.seq = qMax(s.seq, seq);
                break;
            }
        }
        const qint64 previousSid = s.sid++;
        if (!consentOn()) {
            saveStateLocked();
            return;
        }
        QString crashCause;
        if (Logging::PreviousSessionCrashed()) crashCause = queueCrashReportLocked(lines, previousSid, previousStart);

        s.file.setFileName(path("journal.jsonl"));
        s.on = s.file.open(QIODevice::Append);
        saveStateLocked();
        writeLocked("app", "start", "ok",
                    QStringLiteral("v" NKR_VERSION " build %1, %2").arg(SHADOWLOS_VERSION_CODE).arg(osName()));
        if (!crashCause.isEmpty()) writeLocked("app", "previous-crash", "fail", {}, crashCause);
        LOG_INFO(QStringLiteral("[journal] session %1%2").arg(s.sid).arg(crashCause.isEmpty() ? "" : ", crash report queued"));
    }

    void Install(QWidget *window, QMenu *tools, QAbstractButton *tun, QAbstractButton *systemProxy) {
        auto *action = new QAction(translate("Send the error journal to the developers"), tools);
        action->setCheckable(true);
        action->setChecked(consentOn());
        QObject::connect(action, &QAction::toggled, window, [](bool on) { setConsent(on); });
        tools->addAction(action);
        consentAction = action;

        qApp->installEventFilter(new UiTracker(window));

        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, window, [](Qt::ApplicationState state) {
            switch (state) {
                case Qt::ApplicationActive: Event("app", "lifecycle", "ok", QStringLiteral("active")); break;
                case Qt::ApplicationInactive: Event("app", "lifecycle", "ok", QStringLiteral("inactive")); break;
                case Qt::ApplicationHidden: Event("app", "lifecycle", "ok", QStringLiteral("hidden")); break;
                default: Event("app", "lifecycle", "ok", QStringLiteral("suspended")); break;
            }
        });
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] {
            Event("app", "lifecycle", "ok", QStringLiteral("quit"));
            QMutexLocker lock(&st().mutex);
            saveStateLocked();
        });

        const auto watch = [window](QAbstractButton *box, const QString &name) {
            QObject::connect(box, &QAbstractButton::toggled, window, [name](bool on) {
                Event("app", "settings", "ok", name + (on ? QStringLiteral(" on") : QStringLiteral(" off")));
            });
        };
        watch(tun, QStringLiteral("tun"));
        watch(systemProxy, QStringLiteral("system-proxy"));

        auto &u = up();
        u.nam = new QNetworkAccessManager(window);
        u.nam->setProxy(QNetworkProxy::NoProxy);
        u.timer = new QTimer(window);
        u.timer->setInterval(kUploadEveryMs);
        QObject::connect(u.timer, &QTimer::timeout, window, [] { tick(); });
        u.timer->start();
        QTimer::singleShot(kFirstUploadMs, u.timer, [] { tick(); });

        InstallGeo(window);
    }

    QAction *ConsentAction() {
        return consentAction;
    }

    // Free text (journal-format.md: manual is not the journal format): one header line, then
    // the newest journal lines that fit, each scrubbed as JSON. The queue's sent mark is left
    // alone, so the periodic upload still sends these lines as the journal.
    void SendManualReport(std::function<void(ManualResult, int)> done) {
        if (!consentOn()) {
            done(ManualResult::Off, 0);
            return;
        }
        QString token;
        if (!up().nam || !ApiToken(&token)) {
            done(ManualResult::NoToken, 0);
            return;
        }
        Event("app", "report", "ok", QStringLiteral("manual"));
        QStringList lines;
        {
            QMutexLocker lock(&st().mutex);
            lines = readLinesLocked();
        }
        const QString header = QStringLiteral("manual report: the user pressed «Отправить логи» (latest journal lines follow)");
        QStringList tail;
        qsizetype size = header.size() + 1;
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
            const QString line = ScrubJsonLine(*it);
            if (size + line.size() + 1 > kManualReportChars) break;
            size += line.size() + 1;
            tail.prepend(line);
        }
        const QString id = u'm' + QString::fromLatin1(QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'));
        post(QStringLiteral("manual"), id, header + u'\n' + tail.join(u'\n'), [done](int status, qint64) {
            if (status == 200) done(ManualResult::Sent, status);
            else if (status == 429) done(ManualResult::TooOften, status);
            else if (status == 0 || status >= 500) done(ManualResult::NoConnection, status);
            else done(ManualResult::Refused, status);
        }, false);
    }

    void Event(const char *layer, const char *step, const char *outcome, const QString &d, const QString &err, qint64 ms) {
        QMutexLocker lock(&st().mutex);
        writeLocked(layer, step, outcome, d, err, ms);
    }

    int ConnectCount() {
        QMutexLocker lock(&st().mutex);
        return st().connects;
    }

    // From the profile_start/profile_stop hooks: a core crash goes through profile_stop too,
    // which settings->started_id does not follow.
    bool TunnelActive() {
        QMutexLocker lock(&st().mutex);
        return st().connecting || st().upClock.isValid();
    }

    void NetEvent(const QString &d, const QJsonObject &geo) {
        QMutexLocker lock(&st().mutex);
        writeLocked("app", "net", "ok", d, {}, -1, &geo);
    }

    // The Windows shape: platform and os in place of the Android android/sdk keys, which the
    // server refuses from a Windows install.
    QJsonObject UploadEnvelope(const QString &reason, const QString &requestId, const QString &log) {
        QJsonObject body{
            {"app_id", "shadowlos-throne-dev"},
            {"platform", "windows"},
            {"os", osName()},
            {"model", pcModel()},
            {"app_version", NKR_VERSION},
            {"app_build", SHADOWLOS_VERSION_CODE},
            {"reason", reason},
            {"request_id", requestId},
            {"event_time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"consent", true},
            {"log", log},
        };
        if (const QJsonObject geo = SanitizeGeo(LatestGeo()); !geo.isEmpty()) body.insert(QStringLiteral("geo"), geo);
        return body;
    }

    void CoreLog(const QString &text) {
        // Failures as sing-box and Throne print them: "ERROR[0012]", "[Fatal]", "[Core error]", "panic:".
        static const QRegularExpression failRe(QStringLiteral(
            R"(\b(?:ERROR|ERR|FATAL|PANIC)\b|(?i:\[(?:fatal|error|core error)\]|\bpanic\b))"));
        // Warnings ("WARN[0003]", "[Warning] core: Xray … started") are kept outside the
        // connect window too, but as ok: most of them are not failures.
        static const QRegularExpression warnRe(QStringLiteral(R"(\b(?:WARN|WARNING)\b|(?i:\[(?:warn|warning)\]))"));
        static const QRegularExpression coreRe(QStringLiteral(
            R"(\b(?:TRACE|DEBUG|INFO|WARN|WARNING|ERROR|FATAL|PANIC)\b|^panic:|^goroutine |\[Core error\]|\bcore: )"));
        auto &s = st();
        QMutexLocker lock(&s.mutex);
        if (!s.on) return;
        const bool window = s.connectClock.isValid() && s.connectClock.elapsed() < kInfoWindowMs;
        for (const auto &raw : text.split(u'\n')) {
            const QString line = raw.trimmed();
            if (line.isEmpty()) continue;
            const bool fail = failRe.match(line).hasMatch();
            if (!fail && !window && !warnRe.match(line).hasMatch()) continue;
            if (!s.coreMinute.isValid() || s.coreMinute.elapsed() >= 60 * 1000) {
                if (s.coreSkipped > 0) {
                    writeLocked("core-go", "rate-cap", "fail", {},
                                QStringLiteral("%1 log lines skipped (over %2 a minute)").arg(s.coreSkipped).arg(kCoreLinesPerMinute));
                }
                s.coreMinute.start();
                s.coreLines = 0;
                s.coreSkipped = 0;
            }
            if (++s.coreLines > kCoreLinesPerMinute) {
                ++s.coreSkipped;
                continue;
            }
            // GUI lines (subscription, config build) go under app, the core's under core-go.
            const bool core = coreRe.match(line).hasMatch();
            if (fail) {
                s.lastFail = line.left(500);
                writeLocked(core ? "core-go" : "app", core ? "ThroneCore" : "log", "fail", {}, line);
            } else {
                writeLocked(core ? "core-go" : "app", core ? "ThroneCore" : "log", "ok", line);
            }
        }
    }

    void ConnectStart(const QString &server, bool automatic) {
        auto &s = st();
        QMutexLocker lock(&s.mutex);
        s.connecting = true;
        ++s.connects;
        s.connectClock.start();
        s.lastFail.clear();
        writeLocked("app", "server-pick", "ok", automatic ? QStringLiteral("auto") : server);
        writeLocked("app", "state", "ok", QStringLiteral("connecting"));
        writeLocked("app", "connect", "start", server);
        // The net line comes from the UI thread; this runs on a worker.
        if (qApp) QMetaObject::invokeMethod(qApp, [] { GeoConnectStart(); }, Qt::QueuedConnection);
    }

    void ConnectEnd(bool ok) {
        auto &s = st();
        QMutexLocker lock(&s.mutex);
        const qint64 ms = s.connectClock.isValid() ? s.connectClock.elapsed() : -1;
        s.connecting = false;
        if (ok) {
            s.upClock.start();
            writeLocked("app", "connect", "ok", {}, {}, ms);
            writeLocked("app", "state", "ok", QStringLiteral("connected"));
            // An upload once the VPN is up; this runs on a worker thread.
            if (qApp) QMetaObject::invokeMethod(qApp, [] { tick(); }, Qt::QueuedConnection);
        } else {
            writeLocked("app", "connect", "fail", {}, s.lastFail.isEmpty() ? QStringLiteral("start failed") : s.lastFail, ms);
            writeLocked("app", "state", "ok", QStringLiteral("idle"));
        }
    }

    void Disconnect(bool crash, bool manual) {
        auto &s = st();
        QMutexLocker lock(&s.mutex);
        const qint64 ms = s.upClock.isValid() ? s.upClock.elapsed() : -1;
        writeLocked("app", "disconnect", "ok", ms >= 0 ? QStringLiteral("up %1 s").arg(ms / 1000) : QString(),
                    crash ? QStringLiteral("core-exited") : manual ? QStringLiteral("user") : QStringLiteral("stopped"), ms);
        writeLocked("app", "state", "ok", QStringLiteral("idle"));
        s.upClock.invalidate();
    }

} // namespace Shadowlos::Journal

#endif // SHADOWLOS_DEV
