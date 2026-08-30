#include "LocalHiScores.h"
#include "MameLiveClient.h"

#include "Configuration.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <sstream>
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
        std::lock_guard<std::mutex> lock(contextMutex_);
        openhi2txtContext_.reset();
    }
    hiFilesDirectory_.clear();
    scoresDirectory_.clear();
    LOG_INFO("LocalHiScores", "Local high scores deinitialized and cache cleared.");
}

void LocalHiScores::loadHighScores(const std::string& zipPath, const std::string& overridePath) {
    endLiveSession();
    hiFilesDirectory_ = Utils::combinePath(Configuration::absolutePath, "emulators", "mame", "hiscore");
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
    LOG_INFO("LocalHiScores", "OpenHi2txt local cache bulk-loaded " + std::to_string(loaded) + " games.");
}

HighScoreSnapshot LocalHiScores::getTable(const LocalScoreQuery& query) const {
    std::shared_lock<std::shared_mutex> lock(scoresCacheMutex_);
    auto it = scoresCache_.find(query.gameName);
    if (it == scoresCache_.end()) return {};
    return it->second;
}

uint64_t LocalHiScores::getRevision(const std::string& gameName) const {
    std::shared_lock<std::shared_mutex> lock(scoresCacheMutex_);
    auto it = scoresCache_.find(gameName);
    return it == scoresCache_.end() ? 0 : it->second.revision;
}

bool LocalHiScores::hasHiFile(const std::string& gameName) const {
    std::lock_guard<std::mutex> lock(contextMutex_);
    if (openhi2txtContext_) return openhi2txtContext_->hasInputForGame(gameName);
    return std::filesystem::exists(Utils::combinePath(hiFilesDirectory_, gameName + ".hi"));
}

bool LocalHiScores::runHi2Txt(const std::string& gameName) {
    openhi2txt::HiScoreResult result;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!openhi2txtContext_) {
            LOG_ERROR("LocalHiScores", "OpenHi2txt context is not initialized; cannot refresh " + gameName);
            return false;
        }
        if (!openhi2txtContext_->hasInputForGame(gameName)) {
            LOG_INFO("LocalHiScores", "No hi/nvram input exists for " + gameName + ", skipping OpenHi2txt refresh.");
            return false;
        }
        result = openhi2txtContext_->refreshGame(gameName);
    }
    if (!result.ok) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt refresh failed for " + gameName + ": " + result.error);
        return false;
    }

    HighScoreView data = toHighScoreView(result);
    if (data.tables.empty()) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt produced no display tables for " + gameName);
        return false;
    }
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        auto& snapshot = scoresCache_[gameName];
        if (!viewsEqual(snapshot.view, data)) {
            snapshot.view = std::move(data);
            snapshot.origin = HighScoreUpdateOrigin::FileRefresh;
            ++snapshot.revision;
        }
    }
    LOG_INFO("LocalHiScores", "Scores updated for " + gameName + " using OpenHi2txt.");
    return true;
}

void LocalHiScores::runHi2TxtAsync(const std::string& gameName) {
    if (!hasHiFile(gameName)) {
        LOG_INFO("LocalHiScores", "No hi/nvram input exists for " + gameName + ", skipping async OpenHi2txt refresh.");
        return;
    }
    std::thread([this, gameName]() {
        try {
            if (runHi2Txt(gameName)) {
                LOG_INFO("LocalHiScores", "OpenHi2txt refresh executed successfully in the background for game " + gameName);
            } else {
                LOG_ERROR("LocalHiScores", "OpenHi2txt refresh failed in the background for game " + gameName);
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("LocalHiScores", "Exception in async OpenHi2txt refresh for game " + gameName + ": " + e.what());
        }
        catch (...) {
            LOG_ERROR("LocalHiScores", "Unknown exception in async OpenHi2txt refresh for game " + gameName);
        }
    }).detach();
}

bool LocalHiScores::beginLiveSession(const std::string& gameName, std::uint16_t port) {
    endLiveSession();

    std::string expectedSource;
    std::vector<MameLiveWatchRequest> watchRequests;

    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!openhi2txtContext_) {
            LOG_WARNING("LocalHiScores", "OpenHi2txt context is not initialized; live scores are unavailable for " + gameName + ".");
            return false;
        }

        const openhi2txt::HiScoreInputPlanResult plan = openhi2txtContext_->planGameInputs(gameName);
        if (!plan.ok) {
            LOG_INFO("LocalHiScores", "No OpenHi2txt definition is available for live scores for " + gameName + ".");
            return false;
        }

        const bool hasHiInput = std::any_of(plan.inputs.begin(), plan.inputs.end(), [](const auto& input) {
            return input.fileKind.empty() || input.fileKind == "hi" || input.fileKind == ".hi";
        });
        if (hasHiInput) {
			expectedSource = ".hi";
		}
		else {
			for (const auto& input : plan.inputs) {
				const std::string& kind = input.fileKind;
				const bool logicalDisk = kind == "dif" || kind == "chd";
				const bool ordinaryNvram = !logicalDisk && input.acceptedBufferSizes.size() == 1;
				if (kind.empty() || kind == "hi" || kind == ".hi" || kind == "game" ||
					(!ordinaryNvram && (!logicalDisk || input.sourceWindowLength == 0)) ||
					input.watchRanges.empty()) {
					continue;
				}
				MameLiveWatchRequest request;
				request.source = kind;
				request.storage = logicalDisk ? "harddisk" : "nvram";
				request.sourceOffset = logicalDisk ? input.sourceWindowOffset : 0;
				request.sourceSize = logicalDisk
					? input.sourceWindowLength
					: input.acceptedBufferSizes.front();
				request.ranges.reserve(input.watchRanges.size());
				for (const auto& range : input.watchRanges)
					request.ranges.push_back({range.offset, range.length});
				watchRequests.push_back(std::move(request));
			}
		}
        if (expectedSource.empty() && watchRequests.empty()) {
            LOG_INFO("LocalHiScores", "The current live MAME client does not yet support the input type required by " + gameName + ".");
            return false;
        }
    }

	if (!watchRequests.empty()) {
		std::ostringstream detail;
		for (std::size_t index = 0; index < watchRequests.size(); ++index) {
			if (index) detail << ", ";
			detail << watchRequests[index].source << '/' << watchRequests[index].storage << ':'
			       << watchRequests[index].sourceOffset << '+' << watchRequests[index].sourceSize
			       << " (" << watchRequests[index].ranges.size() << " range(s))";
		}
		LOG_INFO("LocalHiScores", "Prepared live source candidates for " + gameName + ": " + detail.str() + ".");
	}
	else {
		LOG_INFO("LocalHiScores", "Prepared live " + expectedSource + " session for " + gameName + ".");
	}

    liveClient_ = std::make_unique<MameLiveClient>(
        gameName,
        expectedSource,
        port,
        std::move(watchRequests),
        [this](MameLiveSnapshot snapshot) { applyLiveSnapshot(std::move(snapshot)); });
    {
        std::lock_guard<std::mutex> lock(liveStateMutex_);
        liveBaselineGame_ = gameName;
        awaitingLiveBaseline_ = true;
    }
    liveClient_->start();
    return true;
}

void LocalHiScores::endLiveSession() {
    if (liveClient_) {
        liveClient_->stop();
        liveClient_.reset();
    }
    std::lock_guard<std::mutex> lock(liveStateMutex_);
    liveBaselineGame_.clear();
    awaitingLiveBaseline_ = false;
}

void LocalHiScores::applyLiveSnapshot(MameLiveSnapshot snapshot) {
	LOG_INFO("LocalHiScores", "Received live snapshot for " + snapshot.game +
		" from " + snapshot.source + " at sequence " + std::to_string(snapshot.sequence) + ".");
    openhi2txt::HiScoreResult result;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!openhi2txtContext_) return;
        const std::string sourceName = "mame://127.0.0.1/live/" +
            std::to_string(snapshot.session) + "/" + std::to_string(snapshot.sequence);
        if (!snapshot.ranges.empty()) {
            openhi2txt::HiScoreSparseInput input;
            input.fileKind = snapshot.source;
            input.sourceOffset = snapshot.sourceOffset;
            input.sourceSize = snapshot.sourceSize;
            input.sourceName = sourceName;
            input.ranges.reserve(snapshot.ranges.size());
            for (auto& range : snapshot.ranges)
                input.ranges.push_back({range.offset, std::move(range.bytes)});
            result = openhi2txtContext_->decodeSparseGame(snapshot.game, {std::move(input)});
        }
        else {
            std::vector<openhi2txt::HiScoreInput> inputs;
            inputs.push_back({snapshot.source, std::move(snapshot.bytes), sourceName});
            result = openhi2txtContext_->decodeGame(snapshot.game, inputs);
        }
    }

    if (!result.ok) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt rejected a live snapshot for " + snapshot.game + ": " + result.error);
        return;
    }

    HighScoreView data = toHighScoreView(result);
    if (data.tables.empty()) {
        LOG_WARNING("LocalHiScores", "OpenHi2txt produced no display tables from a live snapshot for " + snapshot.game + ".");
        return;
    }

    HighScoreUpdateOrigin origin = HighScoreUpdateOrigin::LiveChange;
    {
        std::lock_guard<std::mutex> lock(liveStateMutex_);
        if (awaitingLiveBaseline_ && liveBaselineGame_ == snapshot.game) {
            origin = HighScoreUpdateOrigin::LiveBaseline;
            awaitingLiveBaseline_ = false;
        }
    }

    bool changed = false;
    std::uint64_t revision = 0;
    {
        std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
        auto& cached = scoresCache_[snapshot.game];
        if (!viewsEqual(cached.view, data)) {
            cached.view = std::move(data);
            cached.origin = origin;
            revision = ++cached.revision;
            changed = true;
        }
    }
    if (changed) {
        LOG_INFO("LocalHiScores", "Live scores updated for " + snapshot.game + " at revision " + std::to_string(revision) + ".");
    }
	else {
		LOG_INFO("LocalHiScores", "Live snapshot decoded for " + snapshot.game + "; displayed tables were unchanged.");
	}
}
