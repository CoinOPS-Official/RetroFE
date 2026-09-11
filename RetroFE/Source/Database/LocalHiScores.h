#pragma once

#include "HighScoreView.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <openhi2txt/openhi2txt.h>
#include <openhi2txt/mame_live.h>

struct LocalScoreQuery {
    std::string gameName;
    std::string mameMachine;
    std::string mameSoftwareList;
    std::string mameSoftware;

    bool hasMameIdentity() const { return !mameMachine.empty(); }
};

struct LocalHiScoreStorageHint {
    std::string name;
    std::uint64_t size = 0;
};

class LocalHiScores {
public:
    static LocalHiScores& getInstance();

    ~LocalHiScores();

    void loadHighScores(const std::string& zipPath, const std::string& overridePath);
    HighScoreSnapshot getTable(const LocalScoreQuery& query) const;
    uint64_t getRevision(const LocalScoreQuery& query) const;
    bool needsMameStorageHints(const LocalScoreQuery& query) const;
    bool runHi2Txt(const LocalScoreQuery& query);
    void runHi2TxtAsync(LocalScoreQuery query);
    bool beginLiveSession(
        const LocalScoreQuery& query,
        const std::vector<LocalHiScoreStorageHint>& storageHints,
        std::uint16_t port = 32123);
    void endLiveSession();
    void deinitialize();

private:
    LocalHiScores() = default;

    std::string definitionKey(const LocalScoreQuery& query) const;
    void applyLiveUpdate(const std::string& frontendName, openhi2txt::MameLiveUpdate update);

    std::string scoresDirectory_;
    std::unique_ptr<openhi2txt::Context> openhi2txtContext_;
    std::unique_ptr<openhi2txt::MameLiveClient> liveClient_;
    std::unordered_map<std::string, HighScoreSnapshot> scoresCache_;
    mutable std::unordered_map<std::string, std::string> definitionKeyCache_;
    mutable std::mutex contextMutex_;
    mutable std::shared_mutex scoresCacheMutex_;
    mutable std::shared_mutex definitionKeyCacheMutex_;
};
