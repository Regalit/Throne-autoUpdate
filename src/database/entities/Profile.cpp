#include <include/database/entities/Profile.h>

#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QSet>

#include "include/database/GroupsRepo.h"
#include "include/global/Configs.hpp"
#include "include/ui/shadowlos/ShadowlosJournal.hpp"

namespace Configs
{
    namespace {
        struct ServerCheckVerdict {
            bool full = false;
            QString failed;
            QByteArray net; // Journal::FreshNetworkKey when it was made
            QHash<QString, int> services; // ms to the answer, -1 = did not open
        };
        QMutex serverCheckMutex;
        QHash<int, ServerCheckVerdict> serverChecks;

        ServerCheckVerdict serverCheckOf(int id) {
            QMutexLocker locker(&serverCheckMutex);
            return serverChecks.value(id);
        }
    }

    void Profile::SetServerCheck(bool full, const QString &failed, const QByteArray &net,
                                 const QHash<QString, int> &services) const {
        QMutexLocker locker(&serverCheckMutex);
        if (!full) {
            serverChecks.remove(id);
            return;
        }
        serverChecks.insert(id, {true, failed, net, services});
    }

    ServiceState Profile::CheckService(const QString &name) const {
        if (latency <= 0 || !HasFreshServerCheck()) return ServiceState::Unknown;
        const auto services = serverCheckOf(id).services;
        const auto it = services.constFind(name);
        if (it == services.constEnd()) return ServiceState::Unknown;
        if (*it < 0) return ServiceState::Fails;
        return *it > kServiceSlowMs ? ServiceState::Slow : ServiceState::Opens;
    }

    QString Profile::CheckFailed() const {
        return serverCheckOf(id).failed;
    }

    Profile::Profile(Configs::outbound *outbound, const QString &type_)
    {
        if (!type_.isEmpty()) this->type = type_;


        if (outbound != nullptr) {
            this->outbound = std::shared_ptr<Configs::outbound>(outbound);
        }
    }

    void Profile::ClearTestResults() {
        test_country.clear();
        ip_out.clear();
        latency = 0;
        latency_at = 0;
        SetServerCheck(false, {}, {});
        dl_speed.clear();
        ul_speed.clear();
    }

    void Profile::SetLatency(int ms) {
        latency = ms;
        // 0 means "never measured", so a reset must clear the stamp rather than record now.
        latency_at = ms == 0 ? 0 : QDateTime::currentSecsSinceEpoch();
    }

    bool Profile::HasFreshServerCheck() const {
        if (latency == 0) return false;
        const auto verdict = serverCheckOf(id);
        if (!verdict.full) return false;
        if (QDateTime::currentSecsSinceEpoch() - latency_at > kServerCheckFreshSecs) return false;
        return verdict.net == Shadowlos::Journal::CurrentNetworkKey();
    }

    QString Profile::DisplayTestResult() const {
        auto group = dataManager->groupsRepo->GetGroup(gid);
        if (group == nullptr) return "";
        QString result;
        if (!test_country.isEmpty()) result += UNICODE_LRO + CountryCodeToFlag(test_country) + " ";
        if (latency == kLatencyConnectOnly) {
            result = QObject::tr("Connect OK");
            return result;
        }
        // Shadowlos: a number only for a full check that still holds; a quick latency answer or
        // an old result says nothing about whether sites open. Until then the cell stays empty.
        if (!HasFreshServerCheck()) return "";
        if (latency == kLatencyBlocked) return QObject::tr("blocked");
        if (latency < 0) return QObject::tr("no answer");
        result += QString("%1 ms").arg(latency);
        bool showSpeed = group->test_items_to_show == testShowItems::all || group->test_items_to_show == testShowItems::speedOnly;
        bool showIP = group->test_items_to_show == testShowItems::all || group->test_items_to_show == testShowItems::ipOnly;
        if (!dl_speed.isEmpty() && dl_speed != "N/A" && showSpeed) result += " ↓" + dl_speed;
        if (!ul_speed.isEmpty() && ul_speed != "N/A" && showSpeed) result += " ↑" + ul_speed;
        if (!ip_out.isEmpty() && showIP) result += " 🌐" + ip_out;
        return result;
    }

    QColor Profile::DisplayLatencyColor() const {
        if (latency == kLatencyConnectOnly) return Qt::darkCyan;
        if (!HasFreshServerCheck() || latency < 0) return Qt::darkGray;
        return CheckFailed().isEmpty() ? Qt::darkGreen : Qt::darkYellow;
    }

    QString Profile::DisplayTraffic() const {
        if (traffic_downlink + traffic_uplink == 0) return "";
        return UNICODE_LRO + QString("%1↑ %2↓").arg(ReadableSize(traffic_uplink), ReadableSize(traffic_downlink));
    }

    void Profile::ResetTraffic() {
        traffic_downlink = 0;
        traffic_uplink = 0;
    }

        QString ProfileFilter_ent_key(const std::shared_ptr<Configs::Profile> &ent, bool ignoreMetadata) {
        auto key = ent->outbound->ExportJsonLink(ignoreMetadata);
        return key;
    }

    QString ProfileFilter_ent_identity_key(const std::shared_ptr<Configs::Profile> &ent) {
        auto obj = ent->outbound->ExportIdentity();
        return ent->type + "|" + QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    void ProfileFilter::Uniq(const QList<std::shared_ptr<Profile>> &in,
                             QList<std::shared_ptr<Profile>> &out,
                             bool keep_last, bool ignoreMetadata) {
        QMap<QString, std::shared_ptr<Profile>> hashMap;

        for (const auto &ent: in) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            if (hashMap.contains(key)) {
                if (keep_last) {
                    out.removeAll(hashMap[key]);
                    hashMap[key] = ent;
                    out += ent;
                }
            } else {
                hashMap[key] = ent;
                out += ent;
            }
        }
    }

    void ProfileFilter::Common(const QList<std::shared_ptr<Profile>> &src,
                               const QList<std::shared_ptr<Profile>> &dst,
                               QList<std::shared_ptr<Profile>> &outSrc,
                               QList<std::shared_ptr<Profile>> &outDst,
                               bool ignoreMetadata) {
        QMap<QString, QList<std::shared_ptr<Profile>>> srcByKey;

        for (const auto &ent: src) {
            srcByKey[ProfileFilter_ent_key(ent, ignoreMetadata)].append(ent);
        }
        // A src entry can be claimed once: handing the same one to every dst duplicate collapses N identical servers into one profile (#1775).
        for (const auto &ent: dst) {
            auto it = srcByKey.find(ProfileFilter_ent_key(ent, ignoreMetadata));
            if (it == srcByKey.end() || it->isEmpty()) continue;
            outDst += ent;
            outSrc += it->takeFirst();
        }
    }

    void ProfileFilter::OnlyInSrc(const QList<std::shared_ptr<Profile>> &src,
                                  const QList<std::shared_ptr<Profile>> &dst,
                                  QList<std::shared_ptr<Profile>> &out,
                                  bool ignoreMetadata) {
        QMap<QString, bool> hashMap;

        for (const auto &ent: dst) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            hashMap[key] = true;
        }
        for (const auto &ent: src) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            if (!hashMap.contains(key)) out += ent;
        }
    }

    void ProfileFilter::OnlyInSrc_ByPointer(const QList<std::shared_ptr<Profile>> &src,
                                            const QList<std::shared_ptr<Profile>> &dst,
                                            QList<std::shared_ptr<Profile>> &out) {
        for (const auto &ent: src) {
            if (!dst.contains(ent)) out += ent;
        }
    }

    void ProfileFilter::ChangedByIdentity(QList<std::shared_ptr<Profile>> &src,
                                          QList<std::shared_ptr<Profile>> &dst,
                                          QList<std::shared_ptr<Profile>> &changedSrc,
                                          QList<std::shared_ptr<Profile>> &changedDst) {
        QMap<QString, QList<std::shared_ptr<Profile>>> srcByKey;
        for (const auto &ent: src) {
            srcByKey[ProfileFilter_ent_identity_key(ent)].append(ent);
        }

        QSet<Profile *> matchedSrc;
        QList<std::shared_ptr<Profile>> remainingDst;
        for (const auto &ent: dst) {
            auto &bucket = srcByKey[ProfileFilter_ent_identity_key(ent)];
            if (bucket.isEmpty()) {
                remainingDst += ent;
                continue;
            }
            auto srcEnt = bucket.takeFirst();
            changedSrc += srcEnt;
            changedDst += ent;
            matchedSrc.insert(srcEnt.get());
        }

        QList<std::shared_ptr<Profile>> remainingSrc;
        for (const auto &ent: src) {
            if (!matchedSrc.contains(ent.get())) remainingSrc += ent;
        }
        src = remainingSrc;
        dst = remainingDst;
    }
}
