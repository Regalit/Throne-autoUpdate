#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include "include/database/entities/Profile.h"

namespace Configs
{
    // Not "dashboard": that one is the Clash external_ui dir, holding a different UI.
    inline constexpr auto apiDashboardDir = "sb-dashboard";

    class ExtraCoreData
    {
        public:
        QString path;
        QString args;
        QString config;
        bool noLog = false;
    };

    struct TrafficChainGroup {
        QString watchTag;
        QList<std::shared_ptr<Profile>> profiles;
    };

    struct AutoSelectorBuildInfo {
        QString groupTag;
        std::shared_ptr<Profile> profile;
        QList<QPair<QString, std::shared_ptr<Profile>>> members;
    };

    class BuildConfigResult {
    public:
        QString error;
        QJsonObject coreConfig;
        QString tunIPv4CIDR;
        bool isXrayNeeded = false;
        QJsonObject xrayConfig;
        // Opaque full configs, one instance each; never merged into xrayConfig.
        QStringList xrayFullConfigs;
        // Shadowlos: .srs URLs this config reads from Throne's saved copies (RefreshRuleSetFiles).
        QStringList localRuleSetUrls;
        std::shared_ptr<ExtraCoreData> extraCoreData = std::make_shared<ExtraCoreData>();

        QList<TrafficChainGroup> chainGroups;
        QList<AutoSelectorBuildInfo> autoSelectors;
        // Endpoint hop tag -> profile id, so a live status can be named after its profile.
        QMap<QString, int> vpnEndpointProfiles;
        // Every profile the config was built from, chain hops and route members included.
        QSet<int> involvedProfiles;
    };

    class BuildTestConfigResult {
    public:
        QString error;
        QMap<int, QString> fullConfigs;
        QStringList xrayFullConfigs;
        QMap<QString, int> tag2entID;
        QJsonObject coreConfig;
        QJsonObject xrayConfig;
        bool isXrayNeeded = false;
        QStringList outboundTags;
        QString xrayDnsStrategy;
    };

    inline QString get_jsdelivr_link(QString link)
    {
        if(Configs::dataManager->settingsRepo->ruleset_mirror == Mirrors::GITHUB)
            return link;
        if(auto url = QUrl(link); url.isValid() && url.host() == "raw.githubusercontent.com")
        {
            QStringList list = url.path().split('/');
            QString result;
            switch(Configs::dataManager->settingsRepo->ruleset_mirror) {
            case Mirrors::GCORE: result = "https://gcore.jsdelivr.net/gh"; break;
            case Mirrors::QUANTIL: result = "https://quantil.jsdelivr.net/gh"; break;
            case Mirrors::FASTLY: result = "https://fastly.jsdelivr.net/gh"; break;
            case Mirrors::CDN: result = "https://cdn.jsdelivr.net/gh"; break;
            default: result = "https://testingcf.jsdelivr.net/gh";
            }

            int index = 0;
            foreach(QString item, list)
            {
                if(!item.isEmpty())
                {
                    if(index == 2)
                        result += "@" + item;
                    else
                        result += "/" + item;
                    index++;
                }
            }
            return result;
        }
        return link;
    }

    constexpr int warpProfileID = -2408;

    struct PredefinedDNSEntry {
        QString domain;
        QStringList v4;
        QStringList v6;
    };

    // Hosts-file syntax: "<address> <domain> [domain...]", '#' comments, repeated domains accumulate.
    bool ParsePredefinedDNS(const QStringList &lines, QList<PredefinedDNSEntry> &out, QString *error = nullptr);

    // sing-box duration grammar: one or more "<number><unit>" with unit ns/us/ms/s/m/h/d.
    bool IsValidDuration(const QString &text);

    // directDnsOverride replaces the direct DNS setting for this build only; it is not saved.
    std::shared_ptr<BuildConfigResult> BuildSingBoxConfig(const std::shared_ptr<Profile> &ent,
                                                          const QString &directDnsOverride = {});

    // Shadowlos: where Throne keeps its own copy of a .srs rule set. A copy that is there is used
    // instead of the core's download, whatever its age.
    QString LocalRuleSetPath(const QString &url);

    // Shadowlos: downloads the .srs rule sets the core was told to download (coreConfig from
    // BuildSingBoxConfig) over the user's own network, the way a browser does, and saves them at
    // LocalRuleSetPath. For when the core's own download failed (e.g. TUN mode binding the wrong
    // adapter: "The requested address is not valid in its context"). Blocks; not on the UI thread.
    // Empty when every one was saved, otherwise the first error.
    QString FetchRuleSetFiles(const QJsonObject &coreConfig);

    // Shadowlos: renews the saved copies behind these URLs (BuildConfigResult::localRuleSetUrls) that
    // are older than a day, over the user's own network. A failure keeps the saved copy. The new
    // files are read on the next start. Blocks; not on the UI thread.
    void RefreshRuleSetFiles(const QStringList &urls);

    // coreUnreachable is set when a false came from a failed core call rather than from the config.
    bool IsValid(const std::shared_ptr<Profile> &ent, bool *coreUnreachable = nullptr);

    // Eligible: an openvpn/openconnect profile, or a chain whose exit hop is one, never the reverse.
    bool CanBeAuxEndpoint(const std::shared_ptr<Profile> &ent);

    // Hops behind the exit of a chain endpoint that are endpoints themselves, exit-first.
    QList<int> AuxEndpointInnerHops(int endpointProfileID);

    std::shared_ptr<BuildTestConfigResult> BuildTestConfig(const QList<std::shared_ptr<Profile> > &profiles);
}
