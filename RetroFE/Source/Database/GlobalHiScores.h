#pragma once

#include "HighScoreView.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <functional>
#include <atomic>
#include <optional>
#include <mutex>

struct GlobalScoreQuery {
    std::string gameIds;
    std::string scoreTypes;
};

// ---------------- Global (iScored) model ----------------
// Storage-only, minimal: gameId ? { gameName, [ {player,score,date} ] }
struct GlobalRow {
    std::string player;
    std::string score;   // keep as string; interpret at render time
    std::string date;    // "YYYY-MM-DD HH:MM:SS"
};

struct GlobalGame {
    std::string gameId;                  // key
    std::string gameName;                // label for UI page title
    std::vector<GlobalRow> rows;         // flat rows from iScored
    uint64_t contentHash = 0;
};

struct GlobalHiScoreData {
    std::unordered_map<std::string, GlobalGame> byId;  // key = gameId
};

class GlobalHiScores {
public:
    static GlobalHiScores& getInstance();

    // -------- Global (iScored) storage/IO --------
    void setGlobalGameroom(const std::string& gameroom);         // e.g. "myArcadeRoom"
    void setGlobalPersistPath(const std::string& path);           // e.g. "<config>/hi2txt/global_cache.json"
    bool loadGlobalCacheFromDisk();                               // reads compact V3 schema
    bool saveGlobalCacheToDisk() const;

    HighScoreView getTable(const GlobalScoreQuery& query) const;

    // Accessors (storage-only)
    std::optional<GlobalGame> getGameById(const std::string& gameId) const;
    std::vector<std::string> listGlobalIds() const;                          // convenience


    // Update paths (network fetch -> in-memory store). limit: 0 = all rows per game, else cap.
    void refreshGlobalAllFromSingleCallAsync(int limit, std::function<void()> onFinish);                     // /getAllScores, then group by id
    void refreshGlobalByIdsAsync(const std::vector<std::string>& gameIds,
        int limit);                                 // refresh a subset

    // Direct upsert (if caller already has rows for an id)
    void upsertIScoredGame(const std::string& gameId,
        const std::string& gameName,
        const std::vector<GlobalRow>& rows);              // replaces that id�s rows

    // Shutdown cleanup (persist if you want, then clear)
    void deinitialize();

private:
    GlobalHiScores() = default;

    // -------- Global internals --------
    std::string iscoredGameroom_;
    std::string globalPersistPath_;
    mutable std::mutex globalPersistMutex_; // serializes path changes and cache IO
    std::atomic<bool> globalRefreshInFlight_{ false };

    GlobalHiScoreData global_;                // canonical global store (by gameId)
    mutable std::shared_mutex globalMutex_;   // guards 'global_'

    // HTTP helpers (defined in .cpp; keep header JSON-free)
    static bool httpGet_(const std::string& url, std::string& body, std::string& err);
    static std::string urlEncode_(const std::string& s);

    void ensureEmptyGames_(const std::vector<std::pair<std::string, std::string>>& all);

    // Internal ingestion helpers (parse JSON text & fill 'global_')
    void ingestIScoredAll_(const std::string& jsonText, int capPerGame);
    void ingestIScoredSingle_(const std::string& jsonText,
        const std::string& expectedGameIdOrEmpty,
        int capPerGame);
    bool fetchAllGameIds_(std::vector<std::pair<std::string, std::string>>& out, std::string& err);
    // Small utility to cap rows per game (0 = no cap)
    static void capRows_(std::vector<GlobalRow>& rows, int limit);
};
