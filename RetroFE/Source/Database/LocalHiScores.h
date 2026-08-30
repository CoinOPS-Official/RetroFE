#pragma once

#include "HighScoreView.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <openhi2txt/openhi2txt.h>

struct LocalScoreQuery {
    std::string gameName;
};

class MameLiveClient;
struct MameLiveSnapshot;

class LocalHiScores {
public:
    static LocalHiScores& getInstance();

    ~LocalHiScores();

    void loadHighScores(const std::string& zipPath, const std::string& overridePath);
    HighScoreSnapshot getTable(const LocalScoreQuery& query) const;
    uint64_t getRevision(const std::string& gameName) const;
    bool hasHiFile(const std::string& gameName) const;
    bool runHi2Txt(const std::string& gameName);
    void runHi2TxtAsync(const std::string& gameName);
    bool beginLiveSession(const std::string& gameName, std::uint16_t port = 32123);
    void endLiveSession();
    void deinitialize();

private:
    LocalHiScores() = default;

    void applyLiveSnapshot(MameLiveSnapshot snapshot);

    std::string hiFilesDirectory_;
    std::string scoresDirectory_;
    std::unique_ptr<openhi2txt::Context> openhi2txtContext_;
    std::unique_ptr<MameLiveClient> liveClient_;
    std::unordered_map<std::string, HighScoreSnapshot> scoresCache_;
    mutable std::mutex contextMutex_;
    mutable std::mutex liveStateMutex_;
    mutable std::shared_mutex scoresCacheMutex_;
    std::string liveBaselineGame_;
    bool awaitingLiveBaseline_ = false;
};
