#include "HiScores.h"
#include <unordered_map>

// Minimal provider interface expected by HiScores.cpp:
class IScoredProvider {
public:
    virtual ~IScoredProvider() = default;

    // Fetch a single game's global highs into `out`. `limit`: 0=all (provider may apply server-side or we cap locally).
    virtual bool fetchGame(const std::string& gameName, int limit,
        HighScoreData& out, std::string& err) = 0;

    // Fetch all games' global highs into `outByGame` (keyed by display game name).
    // The iScored "getAllScores" endpoint doesn't support `max`; we'll cap locally.
    virtual bool fetchAll(std::unordered_map<std::string, HighScoreData>& outByGame,
        std::string& err) = 0;
};
