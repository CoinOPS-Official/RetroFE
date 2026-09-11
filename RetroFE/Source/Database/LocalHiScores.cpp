#include "LocalHiScores.h"

#include "Configuration.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

namespace {
constexpr const char* kOpenHi2txtObfuscationKey = "s3cReT123!";

HighScoreDisplayLevel toDisplayLevel(openhi2txt::DisplayLevel display) {
    switch (display) {
    case openhi2txt::DisplayLevel::Extra:
        return HighScoreDisplayLevel::Extra;
    case openhi2txt::DisplayLevel::Debug:
        return HighScoreDisplayLevel::Debug;
    case openhi2txt::DisplayLevel::Always:
    default:
        return HighScoreDisplayLevel::Always;
    }
}

HighScoreView toHighScoreView(const openhi2txt::HiScoreResult& result) {
    HighScoreView data;
    data.tables.reserve(result.tables.size());
    for (const auto& sourceTable : result.tables) {
        HighScoreTableView table;
        table.id = sourceTable.id;
        table.columns = sourceTable.columns;
        table.columnInfo.reserve(sourceTable.columnInfo.size());
        for (const auto& sourceColumn : sourceTable.columnInfo) {
            table.columnInfo.push_back({
                sourceColumn.id,
                sourceColumn.source,
                toDisplayLevel(sourceColumn.display)
            });
        }
        table.rows = sourceTable.rows;
        table.isPlaceholder.assign(table.rows.size(), std::vector<bool>(table.columns.size(), false));
        data.tables.push_back(std::move(table));
    }
    return data;
}

bool viewsEqual(const HighScoreView& lhs, const HighScoreView& rhs) {
    if (lhs.tables.size() != rhs.tables.size()) return false;
    for (size_t i = 0; i < lhs.tables.size(); ++i) {
        const auto& a = lhs.tables[i];
        const auto& b = rhs.tables[i];
        if (a.id != b.id || a.columns != b.columns || a.columnInfo != b.columnInfo || a.rows != b.rows ||
            a.isPlaceholder != b.isPlaceholder) {
            return false;
        }
    }
    return true;
}

}

LocalHiScores& LocalHiScores::getInstance() {
    static LocalHiScores instance;
    return instance;
}

LocalHiScores::~LocalHiScores() {
    endLiveSession();
}

void LocalHiScores::deinitialize() {
    endLiveSession();
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        scoresCache_.clear();
    }
    {
        std::unique_lock<std::shared_mutex> lock(definitionKeyCacheMutex_);
        definitionKeyCache_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        openhi2txtContext_.reset();
    }
    scoresDirectory_.clear();
    LOG_INFO("LocalHiScores", "Local high scores deinitialized and cache cleared.");
}

void LocalHiScores::loadHighScores(const std::string& zipPath, const std::string& overridePath) {
    endLiveSession();
    scoresDirectory_ = overridePath;

    openhi2txt::ContextOptions options;
    options.definitionsZip = Utils::combinePath(Configuration::absolutePath, "hi2txt", "hi2txt.zip");
    options.defaultsZip = zipPath;
    options.scoresDirectory = scoresDirectory_;
    options.mameRoot = Utils::combinePath(Configuration::absolutePath, "emulators", "mame");
    options.defaults.obfuscation = openhi2txt::ObfuscationMode::Xor;
    options.defaults.key = kOpenHi2txtObfuscationKey;
    options.scoreCache.obfuscation = openhi2txt::ObfuscationMode::Xor;
    options.scoreCache.key = kOpenHi2txtObfuscationKey;

    std::unordered_map<std::string, openhi2txt::HiScoreResult> persistedScores;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        try {
            openhi2txtContext_ = std::make_unique<openhi2txt::Context>(std::move(options));
        }
        catch (const std::exception& e) {
            LOG_ERROR("LocalHiScores", std::string("Failed to initialize OpenHi2txt: ") + e.what());
            return;
        }
        openhi2txtContext_->prepareMameDefinitionIndex();
        persistedScores = openhi2txtContext_->readAllPersistedGames();
    }

    if (!std::filesystem::exists(overridePath) || !std::filesystem::is_directory(overridePath)) {
        LOG_INFO("LocalHiScores", "Score override directory does not exist yet: " + overridePath);
    }

    int loaded = 0;
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        scoresCache_.clear();
        for (const auto& persistedScore : persistedScores) {
            HighScoreView data = toHighScoreView(persistedScore.second);
            if (data.tables.empty()) continue;
            scoresCache_[persistedScore.first] = { std::move(data), 1 };
            ++loaded;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(definitionKeyCacheMutex_);
        definitionKeyCache_.clear();
    }
    LOG_INFO("LocalHiScores", "OpenHi2txt local cache bulk-loaded " + std::to_string(loaded) + " games.");
}

HighScoreSnapshot LocalHiScores::getTable(const LocalScoreQuery& query) const {
    const std::string key = definitionKey(query);
    std::shared_lock<std::shared_mutex> lock(scoresCacheMutex_);
    auto it = scoresCache_.find(key);
    if (it == scoresCache_.end()) return {};
    return it->second;
}

uint64_t LocalHiScores::getRevision(const LocalScoreQuery& query) const {
    const std::string key = definitionKey(query);
    std::shared_lock<std::shared_mutex> lock(scoresCacheMutex_);
    auto it = scoresCache_.find(key);
    return it == scoresCache_.end() ? 0 : it->second.revision;
}

std::string LocalHiScores::definitionKey(const LocalScoreQuery& query) const {
    if (!query.hasMameIdentity()) return query.gameName;
    std::string identityKey = query.mameMachine + "\x1f" +
        query.mameSoftwareList + "\x1f" + query.mameSoftware;
    std::transform(identityKey.begin(), identityKey.end(), identityKey.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    {
        std::shared_lock<std::shared_mutex> lock(definitionKeyCacheMutex_);
        const auto cached = definitionKeyCache_.find(identityKey);
        if (cached != definitionKeyCache_.end()) return cached->second;
    }

    std::string resolvedKey = query.gameName;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (openhi2txtContext_) {
            const auto resolved = openhi2txtContext_->resolveDefinition({
                query.mameMachine, query.mameSoftwareList, query.mameSoftware
            });
            if (resolved.ok) resolvedKey = resolved.definitionId;
        }
    }
    {
        std::unique_lock<std::shared_mutex> cacheLock(definitionKeyCacheMutex_);
        definitionKeyCache_[std::move(identityKey)] = resolvedKey;
    }
    return resolvedKey;
}

bool LocalHiScores::needsMameStorageHints(const LocalScoreQuery& query) const {
    if (!query.hasMameIdentity()) return false;
    std::lock_guard<std::mutex> lock(contextMutex_);
    if (!openhi2txtContext_) return false;
    const auto plan = openhi2txtContext_->planGameInputs({
        query.mameMachine, query.mameSoftwareList, query.mameSoftware
    });
    if (!plan.ok) return false;
    for (const auto& input : plan.inputs) {
        std::string kind = input.fileKind;
        std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        const bool isHi = kind.empty() || kind == "hi" || kind == ".hi";
        const bool isDisk = kind == "dif" || kind == "chd";
        if (!isHi && !isDisk && kind != "game" &&
            input.acceptedBufferSizes.size() == 1 && !input.watchRanges.empty()) return true;
    }
    return false;
}

bool LocalHiScores::runHi2Txt(const LocalScoreQuery& query) {
    openhi2txt::HiScoreResult result;
    std::string key = query.gameName;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!openhi2txtContext_) {
            LOG_ERROR("LocalHiScores", "OpenHi2txt context is not initialized; cannot refresh " + query.gameName);
            return false;
        }
        if (query.hasMameIdentity()) {
            const openhi2txt::MameRuntimeIdentity identity{
                query.mameMachine, query.mameSoftwareList, query.mameSoftware
            };
            result = openhi2txtContext_->refreshGame(identity);
        }
        else {
            result = openhi2txtContext_->refreshGame(query.gameName);
        }
    }
    if (!result.ok) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt refresh failed for " + query.gameName + ": " + result.error);
        return false;
    }
    if (!result.game.empty()) key = result.game;

    HighScoreView data = toHighScoreView(result);
    if (data.tables.empty()) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt produced no display tables for " + query.gameName);
        return false;
    }
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        auto& snapshot = scoresCache_[key];
        if (!viewsEqual(snapshot.view, data)) {
            snapshot.view = std::move(data);
            snapshot.origin = HighScoreUpdateOrigin::FileRefresh;
            ++snapshot.revision;
        }
    }
    LOG_INFO("LocalHiScores", "Scores updated for " + query.gameName + " using OpenHi2txt.");
    return true;
}

void LocalHiScores::runHi2TxtAsync(LocalScoreQuery query) {
    std::thread([this, query = std::move(query)]() {
        try {
            if (runHi2Txt(query)) {
                LOG_INFO("LocalHiScores", "OpenHi2txt refresh executed successfully in the background for game " + query.gameName);
            } else {
                LOG_ERROR("LocalHiScores", "OpenHi2txt refresh failed in the background for game " + query.gameName);
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("LocalHiScores", "Exception in async OpenHi2txt refresh for game " + query.gameName + ": " + e.what());
        }
        catch (...) {
            LOG_ERROR("LocalHiScores", "Unknown exception in async OpenHi2txt refresh for game " + query.gameName);
        }
    }).detach();
}

bool LocalHiScores::beginLiveSession(
    const LocalScoreQuery& query,
    const std::vector<LocalHiScoreStorageHint>& storageHints,
    std::uint16_t port) {
    endLiveSession();
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!openhi2txtContext_) {
            LOG_WARNING("LocalHiScores", "OpenHi2txt context is not initialized; live scores are unavailable for " + query.gameName + ".");
            return false;
        }
    }

    openhi2txt::MameLiveOptions options;
    options.expectedIdentity = {
        query.hasMameIdentity() ? query.mameMachine : query.gameName,
        query.mameSoftwareList,
        query.mameSoftware
    };
    options.port = port;
    options.storageHints.reserve(storageHints.size());
    for (const auto& hint : storageHints)
        options.storageHints.push_back({hint.name, hint.size});

    liveClient_ = std::make_unique<openhi2txt::MameLiveClient>(
        *openhi2txtContext_, std::move(options),
        [this, frontendName = query.gameName](openhi2txt::MameLiveUpdate update) {
            applyLiveUpdate(frontendName, std::move(update));
        },
        [](openhi2txt::MameLiveDiagnosticLevel level, const std::string& message) {
            switch (level) {
            case openhi2txt::MameLiveDiagnosticLevel::Error:
                LOG_ERROR("LocalHiScores", message);
                break;
            case openhi2txt::MameLiveDiagnosticLevel::Warning:
                LOG_WARNING("LocalHiScores", message);
                break;
            case openhi2txt::MameLiveDiagnosticLevel::Info:
            default:
                LOG_INFO("LocalHiScores", message);
                break;
            }
        });
    liveClient_->start();
    return true;
}

void LocalHiScores::endLiveSession() {
    if (liveClient_) {
        liveClient_->stop();
        liveClient_.reset();
    }
}

void LocalHiScores::applyLiveUpdate(const std::string& frontendName, openhi2txt::MameLiveUpdate update) {
	LOG_INFO("LocalHiScores", "Received decoded live update for " + update.definitionKey +
		" from " + update.source + " at sequence " + std::to_string(update.sequence) + ".");
    HighScoreView data = toHighScoreView(update.result);
    if (data.tables.empty()) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt produced no display tables from a live update for " + frontendName + ".");
        return;
    }

    const HighScoreUpdateOrigin origin = update.reason == openhi2txt::MameLiveUpdateReason::Baseline
        ? HighScoreUpdateOrigin::LiveBaseline : HighScoreUpdateOrigin::LiveChange;

    bool changed = false;
    std::uint64_t revision = 0;
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        auto& cached = scoresCache_[update.definitionKey];
        if (!viewsEqual(cached.view, data)) {
            cached.view = std::move(data);
            cached.origin = origin;
            revision = ++cached.revision;
            changed = true;
        }
    }
    if (changed) {
        LOG_INFO("LocalHiScores", "Live scores updated for " + frontendName + " at revision " + std::to_string(revision) + ".");
    }
	else {
		LOG_INFO("LocalHiScores", "Live snapshot decoded for " + frontendName + "; displayed tables were unchanged.");
	}
}
