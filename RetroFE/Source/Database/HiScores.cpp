#define NOMINMAX
#ifdef WIN32
#include <Windows.h>
#else
#include <cstring>
#endif

#include "HiScores.h"
#include "../Utility/Utils.h"
#include "../Utility/Log.h"
#include "../Collection/Item.h" 
#include "SDL2/SDL.h"
#include "SDL_image.h"
#include "minizip/unzip.h"
#include "qrcodegen.hpp"
#include "rapidxml.hpp"
#include "rapidxml_utils.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <thread>
#include <random>
#include <atomic>          // std::atomic
#include <shared_mutex>    // std::shared_mutex, std::shared_lock, std::unique_lock
#include <unordered_map>   // std::unordered_map
#include <unordered_set>   // std::unordered_set
#include <cctype>          // std::isdigit, std::toupper
#include <cerrno>          // errno
#include <cstdlib>         // std::strtod, std::strtoll, std::llabs (include on ALL platforms)

#include <numeric>
#include <climits>
#include <locale>
#include <ctime>
#include <iterator>

#include <nlohmann/json.hpp> // single-header JSON
#include <curl/curl.h>       // libcurl (replace if you have your own HTTP)
using json = nlohmann::json;
using qrcodegen::QrCode;

// --- Single-flight guard so we don't run multiple QR passes concurrently ---
static std::atomic<bool> gQrEnsureRunning{ false };


enum class GlobalSort {
	ScoreDesc, ScoreAsc, TimeAsc, TimeDesc, MoneyDesc, MoneyAsc,

	// distance (ascending/descending)
	DistanceCmAsc, DistanceCmDesc,
	DistanceMAsc, DistanceMDesc,
	DistanceKmAsc, DistanceKmDesc,
	DistanceMilesAsc, DistanceMilesDesc,
	DistanceCmMAsc, DistanceCmMDesc,           // input=cm, display "Xm Ycm"
	DistanceInAsc, DistanceInDesc,
	DistanceFtAsc, DistanceFtDesc,
	DistanceFtInAsc, DistanceFtInDesc,         // input=in, display "Xft Yin"
	DistanceYdAsc, DistanceYdDesc,

	// weight
	WeightGAsc, WeightGDesc,
	WeightKgAsc, WeightKgDesc,
	WeightKgGAsc, WeightKgGDesc,                // input=g, display "Xkg Yg"

	DivideBy10Asc, DivideBy10Desc,
	DivideBy100Asc, DivideBy100Desc,
	DivideBy1000Asc, DivideBy1000Desc,
	MultiplyBy10Asc, MultiplyBy10Desc,
	MultiplyBy100Asc, MultiplyBy100Desc,
	MultiplyBy1000Asc, MultiplyBy1000Desc,
};

struct SortCfg {
	GlobalSort mode;
	bool hasDpOverride;
	int  dpOverride;           // valid iff hasDpOverride==true
};

static inline bool isScaledScoreMode_(GlobalSort m) {
	switch (m) {
		case GlobalSort::DivideBy10Asc:   case GlobalSort::DivideBy10Desc:
		case GlobalSort::DivideBy100Asc:  case GlobalSort::DivideBy100Desc:
		case GlobalSort::DivideBy1000Asc: case GlobalSort::DivideBy1000Desc:
		case GlobalSort::MultiplyBy10Asc: case GlobalSort::MultiplyBy10Desc:
		case GlobalSort::MultiplyBy100Asc: case GlobalSort::MultiplyBy100Desc:
		case GlobalSort::MultiplyBy1000Asc: case GlobalSort::MultiplyBy1000Desc:
		return true;
		default: return false;
	}
}

// default decimals when NO dp override is provided
static inline int scoreScaleDefaultDecimals_(GlobalSort m) {
	switch (m) {
		case GlobalSort::DivideBy10Asc:
		case GlobalSort::DivideBy10Desc:    return 1;
		case GlobalSort::DivideBy100Asc:
		case GlobalSort::DivideBy100Desc:   return 2;
		case GlobalSort::DivideBy1000Asc:
		case GlobalSort::DivideBy1000Desc:  return 3;
		case GlobalSort::MultiplyBy10Asc:
		case GlobalSort::MultiplyBy10Desc:
		case GlobalSort::MultiplyBy100Asc:
		case GlobalSort::MultiplyBy100Desc:
		case GlobalSort::MultiplyBy1000Asc:
		case GlobalSort::MultiplyBy1000Desc:
		return 0;
		default: return 0;
	}
}

static inline std::string trim_(std::string s) {
	auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
	while (!s.empty() && isws(s.front())) s.erase(s.begin());
	while (!s.empty() && isws(s.back()))  s.pop_back();
	return s;
}

static inline GlobalSort parseSort_(std::string s) {
	s = Utils::toLower(trim_(s));

	// existing
	if (s == "ascending" || s == "asc") return GlobalSort::ScoreAsc;
	if (s == "timeascending" || s == "time-ascending" || s == "timeasc" || s == "time-asc") return GlobalSort::TimeAsc;
	if (s == "timedescending" || s == "time-descending" || s == "timedesc" || s == "time-desc") return GlobalSort::TimeDesc;
	if (s == "moneyascending" || s == "money-ascending" || s == "moneyasc" || s == "money-asc") return GlobalSort::MoneyAsc;
	if (s == "moneydescending" || s == "money-descending" || s == "moneydesc" || s == "money-desc") return GlobalSort::MoneyDesc;

	// distance (accept both UK/US spelling "metres/meters" and "kilometers/kilometres")
	if (s == "distancecmascending")       return GlobalSort::DistanceCmAsc;
	if (s == "distancecmdescending")      return GlobalSort::DistanceCmDesc;

	if (s == "distancemetresascending" || s == "distancemetersascending")
		return GlobalSort::DistanceMAsc;
	if (s == "distancemetresdescending" || s == "distancemetersdescending")
		return GlobalSort::DistanceMDesc;

	if (s == "distancekmascending" || s == "distancekilometersascending" || s == "distancekilometresascending")
		return GlobalSort::DistanceKmAsc;
	if (s == "distancekmdescending" || s == "distancekilometersdescending" || s == "distancekilometresdescending")
		return GlobalSort::DistanceKmDesc;

	if (s == "distancemilesascending")    return GlobalSort::DistanceMilesAsc;
	if (s == "distancemilesdescending")   return GlobalSort::DistanceMilesDesc;

	if (s == "distancecmandmetresascending" || s == "distancecmandmetersascending")
		return GlobalSort::DistanceCmMAsc;
	if (s == "distancecmandmetresdescending" || s == "distancecmandmetersdescending")
		return GlobalSort::DistanceCmMDesc;

	if (s == "distanceinchesascending")   return GlobalSort::DistanceInAsc;
	if (s == "distanceinchesdescending")  return GlobalSort::DistanceInDesc;

	if (s == "distancefeetascending")     return GlobalSort::DistanceFtAsc;
	if (s == "distancefeetdescending")    return GlobalSort::DistanceFtDesc;

	if (s == "distancefeetinchesascending")  return GlobalSort::DistanceFtInAsc;
	if (s == "distancefeetinchesdescending") return GlobalSort::DistanceFtInDesc;

	if (s == "distanceyardsascending")    return GlobalSort::DistanceYdAsc;
	if (s == "distanceyardsdescending")   return GlobalSort::DistanceYdDesc;

	// weight
	if (s == "weightgramsascending")      return GlobalSort::WeightGAsc;
	if (s == "weightgramsdescending")     return GlobalSort::WeightGDesc;

	if (s == "weightkilogramsascending")  return GlobalSort::WeightKgAsc;
	if (s == "weightkilogramsdescending") return GlobalSort::WeightKgDesc;

	if (s == "weightkilogramsandgramsascending")  return GlobalSort::WeightKgGAsc;
	if (s == "weightkilogramsandgramsdescending") return GlobalSort::WeightKgGDesc;

	// scaled score (divide/multiply)
	if (s == "divideby10ascending" || s == "divideby10asc")  return GlobalSort::DivideBy10Asc;
	if (s == "divideby10descending" || s == "divideby10desc") return GlobalSort::DivideBy10Desc;

	if (s == "divideby100ascending" || s == "divideby100asc")  return GlobalSort::DivideBy100Asc;
	if (s == "divideby100descending" || s == "divideby100desc") return GlobalSort::DivideBy100Desc;

	if (s == "divideby1000ascending" || s == "divideby1000asc")  return GlobalSort::DivideBy1000Asc;
	if (s == "divideby1000descending" || s == "divideby1000desc") return GlobalSort::DivideBy1000Desc;

	if (s == "multiplyby10ascending" || s == "multiplyby10asc")  return GlobalSort::MultiplyBy10Asc;
	if (s == "multiplyby10descending" || s == "multiplyby10desc") return GlobalSort::MultiplyBy10Desc;

	if (s == "multiplyby100ascending" || s == "multiplyby100asc") return GlobalSort::MultiplyBy100Asc;
	if (s == "multiplyby100descending" || s == "multiplyby100desc") return GlobalSort::MultiplyBy100Desc;

	if (s == "multiplyby1000ascending" || s == "multiplyby1000asc") return GlobalSort::MultiplyBy1000Asc;
	if (s == "multiplyby1000descending" || s == "multiplyby1000desc") return GlobalSort::MultiplyBy1000Desc;

	return GlobalSort::ScoreDesc; // default
}


// parse a token like "divideby10ascending2dp" -> {DivideBy10Asc, hasDp=true, dpOverride=2}
static inline SortCfg parseSortAndDp_(std::string token) {
	token = Utils::toLower(trim_(token));
	SortCfg cfg{ GlobalSort::ScoreDesc, false, 0 };

	// Look for trailing "<digits>dp"
	int dp = 0;
	bool hasDp = false;
	if (token.size() >= 3 && token.rfind("dp") == token.size() - 2) {
		// scan left to collect digits before "dp"
		size_t endDigits = token.size() - 2;
		size_t i = endDigits;
		while (i > 0 && std::isdigit((unsigned char)token[i - 1])) --i;
		if (i < endDigits) {
			try {
				dp = std::stoi(token.substr(i, endDigits - i));
				dp = std::max(0, std::min(dp, 9)); // clamp to something reasonable
				hasDp = true;
				token.erase(i, (endDigits - i) + 2); // remove "<digits>dp"
				token = trim_(token);
			}
			catch (...) {}
		}
	}

	cfg.mode = parseSort_(token);        // base token -> mode
	cfg.hasDpOverride = hasDp && isScaledScoreMode_(cfg.mode); // only honored for scaled types
	cfg.dpOverride = dp;
	return cfg;
}


// ---------- QR + Shortener helpers (local to this .cpp) --------------------

static size_t curlWriteToString_(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* out = static_cast<std::string*>(userdata);
	out->append(ptr, size * nmemb);
	return size * nmemb;
}

// One-time init for PNG codec in SDL_image
static void ensureImgPngInit_() {
	static std::once_flag once;
	std::call_once(once, []() {
		if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
			LOG_WARNING("HiScores", std::string("IMG_Init PNG failed: ") + IMG_GetError());
		}
		});
}

// Build a crisp QR surface (integer scale, border in modules). EC=M, bg=#DDDDDD by default.
static SDL_Surface* buildQrSurface_(const std::string& data,
	int requested_px = 58,
	int border_modules = 2,
	Uint8 bgR = 0xDD, Uint8 bgG = 0xDD, Uint8 bgB = 0xDD,
	Uint8 fgR = 0x00, Uint8 fgG = 0x00, Uint8 fgB = 0x00) {
	using qrcodegen::QrCode;
	QrCode qr = QrCode::encodeText(data.c_str(), QrCode::Ecc::MEDIUM);
	const int n = qr.getSize();
	const int total = n + 2 * border_modules;
	int scale = requested_px / total;
	if (scale < 1) scale = 1;
	const int W = total * scale;

	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, W, W, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surf) return nullptr;

	Uint32 bg = SDL_MapRGBA(surf->format, bgR, bgG, bgB, 255);
	Uint32 fg = SDL_MapRGBA(surf->format, fgR, fgG, fgB, 255);
	SDL_FillRect(surf, nullptr, bg);

	// Draw modules as filled rects
	const int off = border_modules * scale;
	SDL_Rect r{ 0,0,scale,scale };
	for (int y = 0; y < n; ++y) {
		for (int x = 0; x < n; ++x) {
			if (!qr.getModule(x, y)) continue;
			r.x = off + x * scale;
			r.y = off + y * scale;
			SDL_FillRect(surf, &r, fg);
		}
	}
	return surf;
}

// Take whatever is.gd gives us; ignore gameId.
static bool isgdShorten_(const std::string& longUrl, const std::string& /*gameId*/, std::string& outShort) {
	// Global throttle: ~1 req/sec
	static std::mutex throttleMtx;
	static std::chrono::steady_clock::time_point lastCall;
	auto throttle = [&]() {
		std::lock_guard<std::mutex> lk(throttleMtx);
		auto now = std::chrono::steady_clock::now();
		if (lastCall.time_since_epoch().count() != 0) {
			auto due = lastCall + std::chrono::milliseconds(1100);
			if (now < due) std::this_thread::sleep_until(due);
		}
		lastCall = std::chrono::steady_clock::now();
		};

	CURL* curl = curl_easy_init();
	if (!curl) return false;

	// Build POST body: format=simple&url=<url-encoded longUrl>
	char* enc = curl_easy_escape(curl, longUrl.c_str(), (int)longUrl.size());
	if (!enc) { curl_easy_cleanup(curl); return false; }
	std::string post = std::string("format=simple&url=") + enc;
	curl_free(enc);

	std::string body;
	long http = 0;
	int sleepSec = 60; // per docs: wait ~1 minute on rate limit

	auto perform = [&]() {
		body.clear();
		curl_easy_setopt(curl, CURLOPT_URL, "https://is.gd/create.php");
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "RetroFE-QR/1.0");
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString_);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // safer in multithreaded apps
		CURLcode rc = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
		return rc == CURLE_OK;
		};

	auto trim = [](std::string& s) {
		auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
		while (!s.empty() && isws((unsigned char)s.front())) s.erase(s.begin());
		while (!s.empty() && isws((unsigned char)s.back()))  s.pop_back();
		};

	for (int attempts = 0; attempts < 8; ++attempts) {
		throttle();
		if (!perform()) { std::this_thread::sleep_for(std::chrono::seconds(5)); continue; }

		trim(body);

		// Success path: HTTP 200 and not "Error: ..."
		if (http == 200 && body.rfind("Error:", 0) != 0 && body.find("is.gd/") != std::string::npos) {
			outShort = body;
			curl_easy_cleanup(curl);
			return true;
		}

		// Handle documented statuses
		if (http == 502 || http == 503) {               // rate limit / service busy
			std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
			continue;
		}
		if (http == 400 || http == 406) {               // bad long URL or (if used) bad custom slug
			curl_easy_cleanup(curl);
			return false;
		}

		// If body starts with "Error:" but status was 200 (some modes do this),
		// check for rate wording and back off, else give up.
		if (body.rfind("Error:", 0) == 0) {
			std::string low = body;
			std::transform(low.begin(), low.end(), low.begin(), ::tolower);
			if (low.find("rate") != std::string::npos || low.find("wait") != std::string::npos) {
				std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
				continue;
			}
			curl_easy_cleanup(curl);
			return false;
		}

		// Unknown hiccup: short nap then retry
		std::this_thread::sleep_for(std::chrono::seconds(5));
	}

	curl_easy_cleanup(curl);
	return false;
}

static void ensureAllQrPngsAsync_(std::vector<std::string> ids) {
	if (gQrEnsureRunning.exchange(true)) {
		LOG_INFO("HiScores", "QR ensure already running; skip new request.");
		return;
	}
	std::thread([ids = std::move(ids)]() {
		try {
			namespace fs = std::filesystem;
			ensureImgPngInit_();

			const std::string qrDir = Utils::combinePath(Configuration::absolutePath, "iScored", "qr");
			std::error_code fec;
			fs::create_directories(qrDir, fec);

			int made = 0, skipped = 0, failed = 0;
			for (const auto& gid : ids) {
				const std::string outPath = Utils::combinePath(qrDir, gid + ".png");
				if (fs::exists(outPath)) { ++skipped; continue; }

				const std::string longUrl = "https://www.iScored.info/?mode=public&user=Pipmick&game=" + gid;
				std::string shortUrl;
				if (!isgdShorten_(longUrl, gid, shortUrl)) {
					++failed;
					LOG_WARNING("HiScores", "QR: shorten failed for " + gid);
					continue;
				}

				SDL_Surface* surf = buildQrSurface_(shortUrl, /*px*/58, /*border*/2,
					/*bg*/0xFF, 0xFF, 0xFF, /*fg*/0x00, 0x00, 0x00);
				if (!surf) {
					++failed;
					LOG_WARNING("HiScores", "QR: surface build failed for " + gid);
					continue;
				}
				if (IMG_SavePNG(surf, outPath.c_str()) != 0) {
					++failed;
					LOG_WARNING("HiScores", std::string("QR: IMG_SavePNG failed for ")
						+ gid + " : " + IMG_GetError());
					SDL_FreeSurface(surf);
					continue;
				}
				SDL_FreeSurface(surf);
				++made;
			}
			LOG_INFO("HiScores", "QR ensure: made=" + std::to_string(made) +
				" skipped=" + std::to_string(skipped) +
				" failed=" + std::to_string(failed));
		}
		catch (const std::exception& e) {
			LOG_ERROR("HiScores", std::string("QR ensure exception: ") + e.what());
		}
		gQrEnsureRunning.store(false);
		}).detach();
}








// --- small helpers local to this .cpp ---
// stringify any JSON scalar
static std::string j2s(const json& v) {
	if (v.is_string()) return v.get<std::string>();
	if (v.is_number_integer())   return std::to_string(v.get<long long>());
	if (v.is_number_unsigned())  return std::to_string(v.get<unsigned long long>());
	if (v.is_number_float())     return std::to_string(v.get<double>());
	return {};
}

static inline std::string rowKey_(const GlobalRow& r) {
	// stable identity for a score row, independent of ordering
	static constexpr char SEP = '\x1F';
	return r.player + SEP + r.score + SEP + r.date;
}

static bool rowsEqualAsSets_(const std::vector<GlobalRow>& a,
	const std::vector<GlobalRow>& b) {
	if (a.size() != b.size()) return false;
	std::vector<std::string> ka; ka.reserve(a.size());
	std::vector<std::string> kb; kb.reserve(b.size());
	for (const auto& r : a) ka.push_back(rowKey_(r));
	for (const auto& r : b) kb.push_back(rowKey_(r));
	std::sort(ka.begin(), ka.end());
	std::sort(kb.begin(), kb.end());
	return ka == kb;
}

void HiScores::capRows_(std::vector<GlobalRow>& rows, int limit) {
	if (limit > 0 && rows.size() > static_cast<size_t>(limit)) {
		rows.resize(static_cast<size_t>(limit));
	}
}

// Get the singleton instance
HiScores& HiScores::getInstance() {
	static HiScores instance;
	return instance;
}

void HiScores::deinitialize() {
	{
		std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);
		scoresCache_.clear();  // Clear all loaded high score data
	}

	hiFilesDirectory_.clear();
	scoresDirectory_.clear();

	LOG_INFO("HiScores", "HiScores deinitialized and cache cleared.");
}

// Load all high scores, first from ZIP, then overriding with external XMLs
void HiScores::loadHighScores(const std::string& zipPath, const std::string& overridePath) {

	hiFilesDirectory_ = Utils::combinePath(Configuration::absolutePath, "emulators", "mame", "hiscore");
	scoresDirectory_ = Utils::combinePath(Configuration::absolutePath, "hi2txt", "scores");

	// Load defaults from the ZIP file
	loadFromZip(zipPath);

	// Check if the override directory exists
	if (std::filesystem::exists(overridePath) && std::filesystem::is_directory(overridePath)) {
		// Load override XML files from the directory
		for (const auto& file : std::filesystem::directory_iterator(overridePath)) {
			if (file.path().extension() == ".xml") {
				std::string gameName = file.path().stem().string();
				std::ifstream fileStream(file.path(), std::ios::binary);
				std::vector<char> buffer((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
				buffer.push_back('\0');

				// Deobfuscate the buffer if necessary
				std::string deobfuscatedContent = Utils::deobfuscate(std::string(buffer.begin(), buffer.end()));
				std::vector<char> deobfuscatedBuffer(deobfuscatedContent.begin(), deobfuscatedContent.end());
				deobfuscatedBuffer.push_back('\0');  // Null-terminate for parsing

				loadFromFile(gameName, file.path().string(), deobfuscatedBuffer);
			}
		}
	}
	else {
		LOG_ERROR("HiScores", "Override directory does not exist or is not accessible: " + overridePath);
	}
}


// Load high scores from XML files within the ZIP archive
void HiScores::loadFromZip(const std::string& zipPath) {
	unzFile zipFile = unzOpen(zipPath.c_str());
	if (zipFile == nullptr) {
		LOG_ERROR("HiScores", "Failed to open ZIP file: " + zipPath);
		return;
	}

	if (unzGoToFirstFile(zipFile) == UNZ_OK) {
		do {
			unz_file_info fileInfo;
			char fileName[256];
			unzGetCurrentFileInfo(zipFile, &fileInfo, fileName, sizeof(fileName), nullptr, 0, nullptr, 0);

			if (std::string(fileName).find(".xml") != std::string::npos) {
				unzOpenCurrentFile(zipFile);

				// Read file content into buffer
				std::vector<char> buffer(fileInfo.uncompressed_size);
				unzReadCurrentFile(zipFile, buffer.data(), fileInfo.uncompressed_size);
				unzCloseCurrentFile(zipFile);

				// Deobfuscate content before parsing
				std::string deobfuscatedContent = Utils::removeNullCharacters(Utils::deobfuscate(std::string(buffer.begin(), buffer.end())));

				// Load deobfuscated data into rapidxml
				std::vector<char> xmlBuffer(deobfuscatedContent.begin(), deobfuscatedContent.end());
				xmlBuffer.push_back('\0');  // Null-terminate for rapidxml

				std::string gameName = std::filesystem::path(fileName).stem().string();
				loadFromFile(gameName, fileName, xmlBuffer);  // Parse and load XML
			}
		} while (unzGoToNextFile(zipFile) == UNZ_OK);
	}

	unzClose(zipFile);
}

// Parse a single XML file for high score data with dynamic columns
void HiScores::loadFromFile(const std::string& gameName, const std::string& filePath, std::vector<char>& buffer) {

	// Ensure the buffer is null-terminated
	buffer.push_back('\0');

	rapidxml::xml_document<> doc;

	try {
		doc.parse<0>(buffer.data());
	}
	catch (const rapidxml::parse_error& e) {
		LOG_ERROR("HiScores", "Parse error in file " + filePath + ": " + e.what());
		return;
	}

	rapidxml::xml_node<> const* rootNode = doc.first_node("hi2txt");
	if (!rootNode) {
		LOG_ERROR("HiScores", "Root node <hi2txt> not found in file " + filePath);
		return;
	}

	HighScoreData highScoreData;

	for (rapidxml::xml_node<> const* tableNode = rootNode->first_node("table"); tableNode; tableNode = tableNode->next_sibling("table")) {
		HighScoreTable highScoreTable;

		// Assign ID if present
		if (tableNode->first_attribute("id")) {
			highScoreTable.id = tableNode->first_attribute("id")->value();
		}

		// Parse columns
		for (rapidxml::xml_node<> const* colNode = tableNode->first_node("col"); colNode; colNode = colNode->next_sibling("col")) {
			highScoreTable.columns.push_back(Utils::trimEnds(colNode->value()));
		}

		// Parse rows
		for (rapidxml::xml_node<> const* rowNode = tableNode->first_node("row"); rowNode; rowNode = rowNode->next_sibling("row")) {
			std::vector<std::string> rowData;
			for (rapidxml::xml_node<> const* cellNode = rowNode->first_node("cell"); cellNode; cellNode = cellNode->next_sibling("cell")) {
				rowData.push_back(Utils::trimEnds(cellNode->value()));
			}
			highScoreTable.rows.push_back(rowData);
		}

		highScoreTable.forceRedraw = true;  // Mark this table for redraw

		highScoreData.tables.push_back(highScoreTable);  // Add the table to the list
	}
	// Lock mutex only for updating the cache
	{
		std::unique_lock<std::shared_mutex> lock(scoresCacheMutex_);  // Exclusive lock for writing
		scoresCache_[gameName] = std::move(highScoreData);  // Update the cache
	}
}

// Retrieve a pointer to the high score table for a specific game
HighScoreData* HiScores::getHighScoreTable(const std::string& gameName) {
	std::shared_lock<std::shared_mutex> lock(scoresCacheMutex_);  // Shared lock for concurrent reads
	auto it = scoresCache_.find(gameName);
	if (it != scoresCache_.end()) {
		return &it->second;
	}
	return nullptr;
}

// Check if a .hi file exists for the given game
bool HiScores::hasHiFile(const std::string& gameName) const {
	std::string hiFilePath = Utils::combinePath(hiFilesDirectory_, gameName + ".hi");
	return std::filesystem::exists(hiFilePath);
}

// Run hi2txt to process the .hi file, generate XML output, save to scores directory, and update cache
bool HiScores::runHi2Txt(const std::string& gameName) {
	// Set up paths
	std::string hi2txtPath;
	std::string hiFilePath = Utils::combinePath(hiFilesDirectory_, gameName + ".hi");

	if (!hasHiFile(gameName)) {
		LOG_INFO("HiScores", ".hi file does not exist for " + gameName + ", skipping async hi2txt.");
		return false;
	}

	// Create the command string
	std::string command;

#ifdef WIN32
	// Windows-specific implementation
	hi2txtPath = Utils::combinePath(Configuration::absolutePath, "hi2txt", "hi2txt");
	command = "\"" + hi2txtPath + "\" -r -xml \"" + hiFilePath + "\"";
	// Initialize structures for the process
	STARTUPINFOA startupInfo;
	PROCESS_INFORMATION processInfo;
	ZeroMemory(&startupInfo, sizeof(startupInfo));
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags |= STARTF_USESTDHANDLES;
	ZeroMemory(&processInfo, sizeof(processInfo));

	// Redirect output to capture it into a buffer
	HANDLE hRead, hWrite;
	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = nullptr;

	if (!CreatePipe(&hRead, &hWrite, &saAttr, 0)) {
		LOG_ERROR("HiScores", "Failed to create pipe.");
		return false;
	}
	startupInfo.hStdOutput = hWrite;
	startupInfo.hStdError = hWrite;

	// Start the process with CREATE_NO_WINDOW to prevent CMD from appearing
	if (!CreateProcessA(
		nullptr,
		const_cast<char*>(command.c_str()),  // Command line
		nullptr, nullptr, TRUE,              // Inherit handles
		CREATE_NO_WINDOW,                    // No window
		nullptr, nullptr, &startupInfo, &processInfo)) {
		LOG_ERROR("HiScores", "Failed to launch hi2txt for game " + gameName);
		CloseHandle(hRead);
		CloseHandle(hWrite);
		return false;
	}

	// Close the write handle and read from the pipe
	CloseHandle(hWrite);
	std::vector<char> buffer;
	char tempBuffer[128];
	DWORD bytesRead;
	while (ReadFile(hRead, tempBuffer, sizeof(tempBuffer), &bytesRead, nullptr) && bytesRead > 0) {
		buffer.insert(buffer.end(), tempBuffer, tempBuffer + bytesRead);
	}
	CloseHandle(hRead);

	// Wait for the process to complete
	DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 5000); // 5 seconds
	if (waitResult == WAIT_TIMEOUT) {
		LOG_ERROR("HiScores", "hi2txt hung for game " + gameName + ", terminating process.");
		TerminateProcess(processInfo.hProcess, 1);
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);
		return false;
	}
	CloseHandle(processInfo.hProcess);
	CloseHandle(processInfo.hThread);

#else
	// Unix-based implementation
	hi2txtPath = Utils::combinePath(Configuration::absolutePath, "hi2txt", "hi2txt.jar");
	command = "java -jar \"" + hi2txtPath + "\" -r -xml \"" + hiFilePath + "\"";
	// Using popen() to execute the command and capture output
	std::vector<char> buffer;
	FILE* pipe = popen(command.c_str(), "r");
	if (!pipe) {
		LOG_ERROR("HiScores", "Failed to run hi2txt command for game " + gameName);
		return false;
	}

	char tempBuffer[128];
	while (fgets(tempBuffer, sizeof(tempBuffer), pipe) != nullptr) {
		buffer.insert(buffer.end(), tempBuffer, tempBuffer + strlen(tempBuffer));
	}

	int returnCode = pclose(pipe);
	if (returnCode != 0) {
		LOG_ERROR("HiScores", "hi2txt process failed with return code " + std::to_string(returnCode));
		return false;
	}
#endif

	// Null-terminate and process the buffer
	buffer.push_back('\0');
	std::string xmlContent(buffer.begin(), buffer.end());

	xmlContent = Utils::removeNullCharacters(xmlContent);
	xmlContent.push_back('\0');  // Ensure null-termination

	// Check if xmlContent starts with <hi2txt>
	if (xmlContent.find("<hi2txt>") != 0) {
		LOG_WARNING("HiScores", "Invalid XML content received from hi2txt for game " + gameName);
		return false;
	}
	// Parse the XML content to update the cache
	std::vector<char> xmlBuffer(xmlContent.begin(), xmlContent.end());
	xmlBuffer.push_back('\0');  // Null-terminate for rapidxml
	loadFromFile(gameName, gameName + ".xml", xmlBuffer);

	// Obfuscate the XML content before saving
	std::string obfuscatedContent = Utils::obfuscate(xmlContent);

	// Save obfuscated XML to the scores directory
	std::string xmlFilePath = Utils::combinePath(scoresDirectory_, gameName + ".xml");
	std::ofstream outFile(xmlFilePath, std::ios::binary);
	if (!outFile) {
		LOG_ERROR("HiScores", "Error: Could not create XML file " + xmlFilePath);
		return false;
	}
	outFile.write(obfuscatedContent.c_str(), obfuscatedContent.size());
	outFile.close();

	LOG_INFO("HiScores", "Scores updated for " + gameName + " and saved to " + xmlFilePath);
	return true;
}

// Wrapper function to run hi2txt asynchronously
void HiScores::runHi2TxtAsync(const std::string& gameName) {
	if (!hasHiFile(gameName)) {
		LOG_INFO("HiScores", ".hi file does not exist for " + gameName + ", skipping async hi2txt.");
		return;
	}
	std::thread([this, gameName]() {
		try {
			if (runHi2Txt(gameName)) {
				LOG_INFO("HiScores", "runHi2Txt executed successfully in the background for game " + gameName);
			}
			else {
				LOG_ERROR("HiScores", "runHi2Txt failed in the background for game " + gameName);
			}
		}
		catch (const std::exception& e) {
			LOG_ERROR("HiScores", "Exception in runHi2TxtAsync for game " + gameName + ": " + e.what());
		}
		catch (...) {
			LOG_ERROR("HiScores", "Unknown exception in runHi2TxtAsync for game " + gameName);
		}
		}).detach();
}

// Helper function to load the XML file content into a buffer
bool HiScores::loadFileToBuffer(const std::string& filePath, std::vector<char>& buffer) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file) {
		LOG_ERROR("HiScores", "Error: Could not open file " + filePath);
		return false;
	}

	// Get the file size
	file.seekg(0, std::ios::end);
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);

	// Resize the buffer to hold the file content
	buffer.resize(size);

	// Read the file content into the buffer
	if (!file.read(buffer.data(), size)) {
		LOG_ERROR("HiScores", "Error: Could not read file content for " + filePath);
		return false;
	}

	return true;
}

static size_t curlWriteCB_(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* s = static_cast<std::string*>(userdata);
	s->append(ptr, size * nmemb);
	return size * nmemb;
}

bool HiScores::httpGet_(const std::string& url, std::string& body, std::string& err) {
	CURL* curl = curl_easy_init();
	if (!curl) { err = "curl_easy_init failed"; return false; }

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "RetroFE-HiScores/1.0");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCB_);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);

	CURLcode rc = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) { err = curl_easy_strerror(rc); return false; }
	if (code < 200 || code >= 300) { err = "HTTP " + std::to_string(code); return false; }
	return true;
}

std::string HiScores::urlEncode_(const std::string& s) {
	std::ostringstream oss;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
		else { oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c << std::nouppercase << std::dec; }
	}
	return oss.str();
}

void HiScores::setGlobalGameroom(const std::string& gameroom) {
	iscoredGameroom_ = gameroom;
}

void HiScores::setGlobalPersistPath(const std::string& path) {
	globalPersistPath_ = path;
}


void HiScores::refreshGlobalAllFromSingleCallAsync(int limit) {
	if (iscoredGameroom_.empty()) {
		LOG_WARNING("HiScores", "refreshGlobalAllFromSingleCallAsync: gameroom not set");
		return;
	}
	std::thread([this, limit]() {
		try {
			// --- Step 1: Fetch the Authoritative Catalog of All Games ---
			// This list is our single source of truth for which games should exist.
			std::vector<std::pair<std::string, std::string>> allIds;
			std::string errIndex;
			if (!fetchAllGameIds_(allIds, errIndex)) {
				LOG_WARNING("HiScores", "Aborting refresh: failed to fetch the authoritative game catalog: " + errIndex);
				return; // Cannot proceed without the master list.
			}

			// --- Step 2: Synchronize Local Cache with the Authoritative Catalog ---
			// Make our 'global_.byId' map perfectly mirror the structure of the server.
			// This adds new empty games, removes games that no longer exist, and updates names.
			{
				std::unique_lock<std::shared_mutex> lk(globalMutex_);
				std::unordered_set<std::string> authoritativeIdSet;
				authoritativeIdSet.reserve(allIds.size());
				for (const auto& pair : allIds) {
					authoritativeIdSet.insert(pair.first);
				}

				// Prune: Remove games from our cache that are not in the authoritative list.
				for (auto it = global_.byId.begin(); it != global_.byId.end(); ) {
					if (authoritativeIdSet.find(it->first) == authoritativeIdSet.end()) {
						it = global_.byId.erase(it);
					}
					else {
						++it;
					}
				}

				// Add & Update: Ensure every game from the catalog exists in our cache and has the correct name.
				for (const auto& pair : allIds) {
					const std::string& gid = pair.first;
					const std::string& gname = pair.second;
					auto it = global_.byId.find(gid);
					if (it == global_.byId.end()) {
						// Add new game, scores will be populated later.
						global_.byId[gid] = GlobalGame{ gid, gname, {} };
					}
					else if (!gname.empty() && it->second.gameName != gname) {
						// Update existing game's name.
						it->second.gameName = gname;
					}
				}
			} // Lock released

			// --- Step 3: Fetch the Score Report ---
			// This payload only contains games with at least one score.
			const std::string url = "https://www.iscored.info/api/" + iscoredGameroom_ + "/getAllScores";
			std::string body, err;
			if (!httpGet_(url, body, err)) {
				LOG_WARNING("HiScores", "Could not fetch score payload: " + err + ". Game list is correct, but scores may be stale.");
				// We don't return here; having a correct game list is still a success.
			}
			else {
				// --- Step 4: Populate the Synced Cache with Scores ---
				// We can use the 'ingestIScoredAll_' logic, which just parses and builds a temporary map.
				// It doesn't need to do any pruning.
				ingestIScoredAll_(body, limit); // This function populates the 'global_' structure internally.
			}


			// --- Step 5: Persist Snapshot of the Correct Cache to Disk ---
			if (!saveGlobalCacheToDisk()) {
				LOG_WARNING("HiScores", "saveGlobalCacheToDisk failed after global update.");
			}

			// --- Step 6: Generate Missing QR Codes ---
			// This now runs on a perfectly correct and synchronized list of games.
			try {
				namespace fs = std::filesystem;
				const std::string qrDir = Utils::combinePath(Configuration::absolutePath, "iScored", "qr");
				std::error_code fec;
				fs::create_directories(qrDir, fec);

				std::vector<std::string> missing;
				{
					std::shared_lock<std::shared_mutex> lk(globalMutex_);
					missing.reserve(global_.byId.size());
					for (const auto& kv : global_.byId) {
						const std::string outPath = Utils::combinePath(qrDir, kv.first + ".png");
						if (!fs::exists(outPath)) missing.push_back(kv.first);
					}
				}
				if (!missing.empty()) {
					ensureAllQrPngsAsync_(std::move(missing));
				}
				else {
					LOG_INFO("HiScores", "QR ensure: nothing missing.");
				}
			}
			catch (const std::exception& e) {
				LOG_ERROR("HiScores", std::string("QR ensure exception: ") + e.what());
			}
		}
		catch (const std::exception& e) {
			LOG_ERROR("HiScores", std::string("refreshGlobalAllFromSingleCallAsync exception: ") + e.what());
		}
		catch (...) {
			LOG_ERROR("HiScores", "refreshGlobalAllFromSingleCallAsync: unknown exception");
		}
		}).detach();
}

bool HiScores::fetchAllGameIds_(std::vector<std::pair<std::string, std::string>>& out, std::string& err) {
	out.clear();
	if (iscoredGameroom_.empty()) { err = "gameroom not set"; return false; }

	const std::string url = "https://www.iscored.info/api/" + iscoredGameroom_;
	std::string body;
	if (!httpGet_(url, body, err)) return false;

	auto strish = [](const nlohmann::json& v) -> std::string {
		if (v.is_string()) return v.get<std::string>();
		if (v.is_number_integer())   return std::to_string(v.get<long long>());
		if (v.is_number_unsigned())  return std::to_string(v.get<unsigned long long>());
		if (v.is_number_float())     return std::to_string(v.get<double>());
		return {};
		};

	try {
		nlohmann::json j = nlohmann::json::parse(body);

		// primary shape you showed: top-level array of objects
		if (j.is_array()) {
			out.reserve(j.size());
			std::unordered_set<std::string> seen;
			for (const auto& g : j) {
				if (!g.is_object()) continue;

				// accept a few id/name variants; no other fields matter here
				std::string gid;
				if (g.contains("gameID")) gid = strish(g["gameID"]);
				else if (g.contains("gameId")) gid = strish(g["gameId"]);
				else if (g.contains("game"))   gid = strish(g["game"]);
				else if (g.contains("id"))     gid = strish(g["id"]);
				if (gid.empty() || seen.count(gid)) continue;

				std::string gname;
				if (g.contains("gameName")) gname = strish(g["gameName"]);
				else if (g.contains("name")) gname = strish(g["name"]);

				out.emplace_back(std::move(gid), std::move(gname));
				seen.insert(out.back().first);
			}
			return !out.empty();
		}

		// tolerant fallbacks (in case the API returns different shapes sometimes)
		if (j.is_object() && j.contains("games") && j["games"].is_array()) {
			for (const auto& g : j["games"]) {
				if (!g.is_object()) continue;
				std::string gid;
				if (g.contains("gameID")) gid = strish(g["gameID"]);
				else if (g.contains("gameId")) gid = strish(g["gameId"]);
				else if (g.contains("game"))   gid = strish(g["game"]);
				else if (g.contains("id"))     gid = strish(g["id"]);
				if (gid.empty()) continue;
				std::string gname = g.contains("gameName") ? strish(g["gameName"])
					: (g.contains("name") ? strish(g["name"]) : "");
				out.emplace_back(std::move(gid), std::move(gname));
			}
			return !out.empty();
		}

		// object mapping id -> object (synthesize id)
		if (j.is_object()) {
			for (auto it = j.begin(); it != j.end(); ++it) {
				if (!it.value().is_object()) continue;
				std::string gid = it.key();
				std::string gname;
				const auto& obj = it.value();
				if (obj.contains("gameName")) gname = strish(obj["gameName"]);
				else if (obj.contains("name")) gname = strish(obj["name"]);
				if (!gid.empty()) out.emplace_back(std::move(gid), std::move(gname));
			}
			return !out.empty();
		}

		err = "unrecognized JSON shape for game index";
		return false;
	}
	catch (const std::exception& e) {
		err = std::string("parse error: ") + e.what();
		return false;
	}
}

// Ensure entries exist for all ids (even with zero rows).
void HiScores::ensureEmptyGames_(const std::vector<std::pair<std::string, std::string>>& all) {
	std::unique_lock<std::shared_mutex> lk(globalMutex_);
	for (const auto& kv : all) {
		const std::string& gid = kv.first;
		const std::string& gname = kv.second;
		auto it = global_.byId.find(gid);
		if (it == global_.byId.end()) {
			GlobalGame gg;
			gg.gameId = gid;
			gg.gameName = gname;
			global_.byId.emplace(gid, std::move(gg));
		}
		else if (!gname.empty() && it->second.gameName != gname) {
			it->second.gameName = gname;
		}
	}
}

void HiScores::ingestIScoredAll_(const std::string& jsonText, int capPerGame) {
	using nlohmann::json;

	json j;
	try {
		j = json::parse(jsonText);
	}
	catch (const std::exception& e) {
		LOG_ERROR("HiScores", std::string("ingestIScoredAll_: JSON parse error: ") + e.what());
		return;
	}

	// --- MODIFICATION ---
	// We will now work directly on the 'global_' object.
	// Lock the mutex for the entire duration of the score population.
	std::unique_lock<std::shared_mutex> lk(globalMutex_);

	// Before we begin, it's safer to clear the scores of all games in the cache.
	// This handles cases where a game's last score was removed, making it empty.
	for (auto& pair : global_.byId) {
		pair.second.rows.clear();
	}

	// Helper function to find a game and push a score row to it.
	// It will only operate on games that are already in our 'global_.byId' map.
	auto pushRow = [&](const std::string& gid, const std::string& gname,
		const json& s) {
			auto it = global_.byId.find(gid);
			if (it == global_.byId.end()) {
				// This shouldn't happen with the new refresh logic, but is a good safeguard.
				return;
			}

			GlobalGame& gg = it->second;
			// Optionally update the game name if the scores payload has a more current one.
			if (!gname.empty() && gg.gameName.empty()) gg.gameName = gname;

			GlobalRow r;
			r.player = s.value("name", "");
			r.score = s.contains("score") ? j2s(s["score"]) : "";
			r.date = s.value("date", "");
			gg.rows.push_back(std::move(r));
		};

	auto parseScoresArray = [&](const std::string& gid, const std::string& gname,
		const json& arr) {
			if (!arr.is_array()) return;
			for (const auto& s : arr) pushRow(gid, gname, s);
		};

	// --- Parsing logic is the same, but now calls the modified 'pushRow' ---

	// Shape A: { "games": [ { "gameId":..., "gameName":..., "scores":[...] }, ... ] }
	if (j.is_object() && j.contains("games") && j["games"].is_array()) {
		for (const auto& g : j["games"]) {
			const std::string gid = g.contains("gameId") ? j2s(g["gameId"]) : "";
			const std::string gname = g.contains("gameName") ? g["gameName"].get<std::string>() : "";
			if (gid.empty()) continue;
			if (g.contains("scores")) parseScoresArray(gid, gname, g["scores"]);
		}
	}
	// Shape B: { "scores": [ { "game":..., "gameName":..., "name":..., "score":..., "date":... }, ... ] }
	else if (j.is_object() && j.contains("scores") && j["scores"].is_array()) {
		for (const auto& s : j["scores"]) {
			const std::string gid = s.contains("game") ? j2s(s["game"]) : "";
			const std::string gname = s.contains("gameName") ? j2s(s["gameName"]) : "";
			if (gid.empty()) continue;
			pushRow(gid, gname, s);
		}
	}
	// ... (other JSON shapes C and D follow the same pattern) ...
	else if (j.is_array()) {
		for (const auto& s : j) {
			if (!s.is_object()) continue;
			const std::string gid = s.contains("game") ? j2s(s["game"]) : "";
			const std::string gname = s.contains("gameName") ? j2s(s["gameName"]) : "";
			if (gid.empty()) continue;
			pushRow(gid, gname, s);
		}
	}
	else if (j.is_object()) {
		for (auto it = j.begin(); it != j.end(); ++it) {
			const std::string key = it.key();
			const json& v = it.value();
			if (v.is_array()) {
				for (const auto& s : v) pushRow(key, /*gname*/"", s);
			}
			else if (v.is_object() && v.contains("scores") && v["scores"].is_array()) {
				const std::string gname = v.contains("gameName") ? j2s(v["gameName"]) : "";
				for (const auto& s : v["scores"]) pushRow(key, gname, s);
			}
		}
	}
	else {
		LOG_WARNING("HiScores", "ingestIScoredAll_: Unrecognized JSON shape");
	}

	// Cap rows per game if requested
	for (auto& kv : global_.byId) {
		capRows_(kv.second.rows, capPerGame);
	}

	// --- MODIFICATION ---
	// The destructive 'global_ = std::move(tmp)' is removed. The lock will be released
	// automatically when the function ends.
}
bool HiScores::loadGlobalCacheFromDisk() {
	if (globalPersistPath_.empty()) return false;

	std::ifstream in(globalPersistPath_);
	if (!in) return false;

	try {
		nlohmann::json root;
		in >> root;

		if (!root.contains("games") || !root["games"].is_array()) {
			LOG_WARNING("HiScores", "loadGlobalCacheFromDisk: no 'games' array.");
			return false;
		}

		GlobalHiScoreData tmp;

		for (const auto& g : root["games"]) {
			if (!g.is_object()) continue;

			GlobalGame gg;
			gg.gameId = g.contains("gameId") ? j2s(g["gameId"]) : "";
			gg.gameName = g.contains("gameName") ? j2s(g["gameName"]) : "";
			if (gg.gameId.empty()) continue;

			if (g.contains("scores") && g["scores"].is_array()) {
				gg.rows.reserve(g["scores"].size());
				for (const auto& s : g["scores"]) {
					if (!s.is_object()) continue;
					GlobalRow r;
					r.player = s.value("name", "");
					r.score = s.contains("score") ? j2s(s["score"]) : "";
					r.date = s.value("date", "");
					gg.rows.push_back(std::move(r));
				}
			}

			tmp.byId.emplace(gg.gameId, std::move(gg));
		}

		std::unique_lock<std::shared_mutex> lk(globalMutex_);
		global_ = std::move(tmp);
		return true;
	}
	catch (const std::exception& e) {
		LOG_ERROR("HiScores", std::string("loadGlobalCacheFromDisk: parse error: ") + e.what());
		return false;
	}
	catch (...) {
		return false;
	}
}

bool HiScores::saveGlobalCacheToDisk() const {
	if (globalPersistPath_.empty()) return false;

	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path p(globalPersistPath_);
	if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

	// Snapshot + sort by gameId (for deterministic output)
	std::vector<GlobalGame> ordered;
	{
		std::shared_lock<std::shared_mutex> lk(globalMutex_);
		ordered.reserve(global_.byId.size());
		for (const auto& kv : global_.byId) ordered.push_back(kv.second);
	}
	std::sort(ordered.begin(), ordered.end(),
		[](const GlobalGame& a, const GlobalGame& b) { return a.gameId < b.gameId; });

	nlohmann::json root; root["version"] = 3;
	auto& games = root["games"] = nlohmann::json::array();
	for (const auto& gg : ordered) {
		nlohmann::json g;
		g["gameId"] = gg.gameId;
		if (!gg.gameName.empty()) g["gameName"] = gg.gameName;
		auto& scores = g["scores"] = nlohmann::json::array();
		for (const auto& r : gg.rows) {
			scores.push_back({ {"name", r.player}, {"score", r.score}, {"date", r.date} });
		}
		games.push_back(std::move(g));
	}

	// Atomic write: to temp then rename
	fs::path tmp = p; tmp += ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary);
		if (!out) return false;
		out << root.dump(2);
		out.flush();
		if (!out) return false;
	}
	fs::rename(tmp, p, ec);
	if (ec) {
		// fallback: direct write
		std::ofstream out(p, std::ios::binary);
		if (!out) return false;
		out << root.dump(2);
		return static_cast<bool>(out);
	}
	return true;
}

// Helpers (local to this .cpp)

// Optional: normalize player names for dedupe (case/space tolerant).

static inline std::string formatThousands_(const std::string& s) {
	if (s.empty()) return s;

	// Accept optional leading sign and optional decimal part. No exponents.
	size_t i = 0;
	bool neg = false;
	if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; if (i >= s.size()) return s; }

	// digits [.] digits?
	size_t startInt = i;
	while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
	size_t endInt = i;

	std::string frac;
	if (i < s.size() && s[i] == '.') {
		size_t dot = i++;
		size_t startFrac = i;
		while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
		// If anything non-digit after frac, bail (e.g., scientific or junk)
		if (i != s.size()) return s;
		frac = s.substr(dot); // includes '.'
	}
	else {
		// If anything non-digit after int, bail
		if (i != s.size()) return s;
	}

	// No integer digits? bail.
	if (endInt == startInt) return s;

	// Insert commas into integer part
	std::string intPart = s.substr(startInt, endInt - startInt);
	std::string outInt;
	outInt.reserve(intPart.size() + intPart.size() / 3);
	int count = 0;
	for (int j = (int)intPart.size() - 1; j >= 0; --j) {
		outInt.push_back(intPart[(size_t)j]);
		if (++count == 3 && j != 0) { outInt.push_back(','); count = 0; }
	}
	std::reverse(outInt.begin(), outInt.end());

	if (neg) return "-" + outInt + frac;
	return outInt + frac;
}



// Normalize player names for dedupe & display: trim + ALL CAPS
static inline std::string normName_(std::string s) {
	s = trim_(s);
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return (char)std::toupper(c); });
	return s;
}



// strict integer parse (±digits only)
static inline bool parseLongLongStrict_(const std::string& s, long long& out) {
	if (s.empty()) return false;
	char* end = nullptr;
	errno = 0;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (errno != 0 || !end || *end != '\0') return false;
	out = v;
	return true;
}

// ---- canonical units (integers) for comparison ----
// distances:
//   cm   base = centimeters
//   m    base = centimeters (m * 100)
//   km   base = centimeters (km * 100000)
//   in   base = inches
//   ft   base = inches (ft * 12)
//   yd   base = inches (yd * 36)
//   miles base = inches (mi * 63360)
//
// weights:
//   g    base = grams
//   kg   base = grams (kg * 1000)

static inline bool toCanonicalDistance_(GlobalSort mode, const std::string& s, long long& out) {
	long long v;
	if (!parseLongLongStrict_(s, v)) return false;

	switch (mode) {
		case GlobalSort::DistanceCmAsc:
		case GlobalSort::DistanceCmDesc:
		case GlobalSort::DistanceCmMAsc:
		case GlobalSort::DistanceCmMDesc:
		out = v; // already cm
		return true;

		case GlobalSort::DistanceMAsc:
		case GlobalSort::DistanceMDesc:
		out = v * 100; // m -> cm
		return true;

		case GlobalSort::DistanceKmAsc:
		case GlobalSort::DistanceKmDesc:
		out = v * 100000; // km -> cm
		return true;

		case GlobalSort::DistanceInAsc:
		case GlobalSort::DistanceInDesc:
		case GlobalSort::DistanceFtInAsc:
		case GlobalSort::DistanceFtInDesc:
		out = v; // already inches
		return true;

		case GlobalSort::DistanceFtAsc:
		case GlobalSort::DistanceFtDesc:
		out = v * 12; // ft -> in
		return true;

		case GlobalSort::DistanceYdAsc:
		case GlobalSort::DistanceYdDesc:
		out = v * 36; // yd -> in
		return true;

		case GlobalSort::DistanceMilesAsc:
		case GlobalSort::DistanceMilesDesc:
		out = v * 63360; // miles -> in
		return true;

		default: return false;
	}
}

static inline bool toCanonicalWeight_(GlobalSort mode, const std::string& s, long long& out) {
	long long v;
	if (!parseLongLongStrict_(s, v)) return false;

	switch (mode) {
		case GlobalSort::WeightGAsc:
		case GlobalSort::WeightGDesc:
		case GlobalSort::WeightKgGAsc:
		case GlobalSort::WeightKgGDesc:
		out = v; // g
		return true;

		case GlobalSort::WeightKgAsc:
		case GlobalSort::WeightKgDesc:
		out = v * 1000; // kg -> g
		return true;

		default: return false;
	}
}

// ---- pretty formatting ----
static inline std::string fmtDistance_(GlobalSort mode, long long canonical) {
	switch (mode) {
		case GlobalSort::DistanceCmAsc:
		case GlobalSort::DistanceCmDesc:
		return formatThousands_(std::to_string(canonical)) + " cm";

		case GlobalSort::DistanceMAsc:
		case GlobalSort::DistanceMDesc:
		return formatThousands_(std::to_string(canonical / 100)) + " m";

		case GlobalSort::DistanceKmAsc:
		case GlobalSort::DistanceKmDesc:
		return formatThousands_(std::to_string(canonical / 100000)) + " km";

		case GlobalSort::DistanceMilesAsc:
		case GlobalSort::DistanceMilesDesc: {
			// canonical=inches; display "X miles"
			long long miles = canonical / 63360;
			return formatThousands_(std::to_string(miles)) + " miles";
		}

		case GlobalSort::DistanceCmMAsc:
		case GlobalSort::DistanceCmMDesc: {
			// canonical=cm; display "Xm Ycm"
			long long m = canonical / 100;
			int cm = (int)(canonical % 100);
			return formatThousands_(std::to_string(m)) + " m " + std::to_string(cm) + " cm";
		}

		case GlobalSort::DistanceInAsc:
		case GlobalSort::DistanceInDesc:
		return formatThousands_(std::to_string(canonical)) + " in";

		case GlobalSort::DistanceFtAsc:
		case GlobalSort::DistanceFtDesc: {
			long long ft = canonical / 12; // canonical=in
			return formatThousands_(std::to_string(ft)) + " ft";
		}

		case GlobalSort::DistanceFtInAsc:
		case GlobalSort::DistanceFtInDesc: {
			long long ft = canonical / 12;
			int in = (int)(canonical % 12);
			return formatThousands_(std::to_string(ft)) + " ft " + std::to_string(in) + " in";
		}

		case GlobalSort::DistanceYdAsc:
		case GlobalSort::DistanceYdDesc: {
			long long yd = canonical / 36; // canonical=in
			return formatThousands_(std::to_string(yd)) + " yds";
		}

		default: return "-";
	}
}

static inline std::string fmtWeight_(GlobalSort mode, long long canonical) {
	switch (mode) {
		case GlobalSort::WeightGAsc:
		case GlobalSort::WeightGDesc:
		return formatThousands_(std::to_string(canonical)) + " g";

		case GlobalSort::WeightKgAsc:
		case GlobalSort::WeightKgDesc: {
			long long kg = canonical / 1000;
			return formatThousands_(std::to_string(kg)) + " kg";
		}

		case GlobalSort::WeightKgGAsc:
		case GlobalSort::WeightKgGDesc: {
			long long kg = canonical / 1000;
			int g = (int)(canonical % 1000);
			return formatThousands_(std::to_string(kg)) + " kg " + std::to_string(g) + " g";
		}

		default: return "-";
	}
}

// mm:ss:ms formatter (zero-padded, ms = 3 digits)
static inline std::string formatMs_(long long ms) {
	if (ms < 0) ms = -ms;
	long long totalSec = ms / 1000;
	int msec = static_cast<int>(ms % 1000);
	long long minutes = totalSec / 60;
	int seconds = static_cast<int>(totalSec % 60);

	std::ostringstream oss;
	oss << minutes  // no padding here
		<< ":" << std::setw(2) << std::setfill('0') << seconds
		<< ":" << std::setw(3) << std::setfill('0') << msec;
	return oss.str();
}

// $ + thousands, preserving sign if any; fallback to raw if not numeric
static inline std::string formatMoney_(const std::string& s) {
	long long v;
	if (!parseLongLongStrict_(s, v)) return s;
	std::string core = formatThousands_(std::to_string(std::llabs(v)));
	return v < 0 ? ("-$" + core) : ("$" + core);
}

static inline bool parseNumber_(const std::string& s, double& out) {
	if (s.empty()) return false;
	char* end = nullptr;
	out = std::strtod(s.c_str(), &end);
	return end && *end == '\0';
}

// parse numeric and apply the scale transform for comparison/formatting
static inline bool getScaledScore_(GlobalSort mode, const std::string& s, double& out) {
	double v;
	if (!parseNumber_(s, v)) return false;
	switch (mode) {
		case GlobalSort::DivideBy10Asc:
		case GlobalSort::DivideBy10Desc:     out = v / 10.0;  return true;
		case GlobalSort::DivideBy100Asc:
		case GlobalSort::DivideBy100Desc:    out = v / 100.0; return true;
		case GlobalSort::DivideBy1000Asc:
		case GlobalSort::DivideBy1000Desc:   out = v / 1000.0; return true;
		case GlobalSort::MultiplyBy10Asc:
		case GlobalSort::MultiplyBy10Desc:   out = v * 10.0;  return true;
		case GlobalSort::MultiplyBy100Asc:
		case GlobalSort::MultiplyBy100Desc:   out = v * 100.0;   return true;
		case GlobalSort::MultiplyBy1000Asc:
		case GlobalSort::MultiplyBy1000Desc:  out = v * 1000.0;  return true;
		default: return false;
	}
}

// format a scaled number with fixed decimals; then add thousands separators
static inline std::string formatScaledScoreStr_(GlobalSort mode, double val, int dpOverride /* -1 => default */) {
	int dp = (dpOverride >= 0) ? dpOverride : scoreScaleDefaultDecimals_(mode);
	std::ostringstream oss;
	oss.setf(std::ios::fixed);
	oss << std::setprecision(dp) << val;
	return formatThousands_(oss.str());
}


// Decide if 'a' is better than 'b' for a given sort mode.
// Returns true if 'a' should replace 'b' as the best row for a name.
static inline bool isBetterForMode_(GlobalSort sortKind,
	const GlobalRow& a,
	const GlobalRow& b) {
	switch (sortKind) {
		case GlobalSort::ScoreAsc:
		case GlobalSort::ScoreDesc: {
			double na, nb;
			bool ha = parseNumber_(a.score, na);
			bool hb = parseNumber_(b.score, nb);
			if (ha && hb) {
				return (sortKind == GlobalSort::ScoreAsc) ? (na < nb) : (na > nb);
			}
			if (ha != hb) return ha; // numeric beats non-numeric
			// Tie-breaker: lexicographic score, then earlier date (so older wins, tweak if desired)
			int c = a.score.compare(b.score);
			if (c != 0) return c < 0;
			return a.date < b.date;
		}
		case GlobalSort::TimeAsc:
		case GlobalSort::TimeDesc: {
			long long ta, tb;
			bool ha = parseLongLongStrict_(a.score, ta);
			bool hb = parseLongLongStrict_(b.score, tb);
			if (ha && hb) {
				return (sortKind == GlobalSort::TimeAsc) ? (ta < tb) : (ta > tb);
			}
			if (ha != hb) return ha; // numeric beats non-numeric
			int c = a.score.compare(b.score);
			if (c != 0) return c < 0;
			return a.date < b.date;
		}
		case GlobalSort::MoneyAsc:
		case GlobalSort::MoneyDesc: {
			long long va, vb;
			bool ha = parseLongLongStrict_(a.score, va);
			bool hb = parseLongLongStrict_(b.score, vb);
			if (ha && hb) {
				return (sortKind == GlobalSort::MoneyAsc) ? (va < vb) : (va > vb);
			}
			if (ha != hb) return ha; // numeric beats non-numeric
			int c = a.score.compare(b.score);
			if (c != 0) return c < 0;
			return a.date < b.date;
		}
		case GlobalSort::DivideBy10Asc:   case GlobalSort::DivideBy10Desc:
		case GlobalSort::DivideBy100Asc:  case GlobalSort::DivideBy100Desc:
		case GlobalSort::DivideBy1000Asc: case GlobalSort::DivideBy1000Desc:
		case GlobalSort::MultiplyBy10Asc: case GlobalSort::MultiplyBy10Desc:
		case GlobalSort::MultiplyBy100Asc: case GlobalSort::MultiplyBy100Desc:
		case GlobalSort::MultiplyBy1000Asc: case GlobalSort::MultiplyBy1000Desc:
		{
			double na, nb;
			bool ha = getScaledScore_(sortKind, a.score, na);
			bool hb = getScaledScore_(sortKind, b.score, nb);
			const bool asc =
				(sortKind == GlobalSort::DivideBy10Asc || sortKind == GlobalSort::DivideBy100Asc ||
					sortKind == GlobalSort::DivideBy1000Asc || sortKind == GlobalSort::MultiplyBy10Asc ||
					sortKind == GlobalSort::MultiplyBy100Asc || sortKind == GlobalSort::MultiplyBy1000Asc);
			if (ha && hb) return asc ? (na < nb) : (na > nb);
			if (ha != hb) return ha; // numeric beats non-numeric
			int c = a.score.compare(b.score);
			if (c != 0) return c < 0;
			return a.date < b.date;
		}
	}
	return false;
}

static inline std::vector<std::string> splitCSV_(const std::string& csv) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : csv) {
		if (c == ',') { cur = trim_(cur); if (!cur.empty()) out.push_back(cur); cur.clear(); }
		else cur.push_back(c);
	}
	cur = trim_(cur); if (!cur.empty()) out.push_back(cur);
	return out;
}

static inline std::string ordinal_(size_t n) {
	size_t x = n % 100;
	if (x >= 11 && x <= 13) return std::to_string(n) + "th";
	switch (n % 10) {
		case 1: return std::to_string(n) + "st";
		case 2: return std::to_string(n) + "nd";
		case 3: return std::to_string(n) + "rd";
		default: return std::to_string(n) + "th";
	}
}
static inline std::string monthName_(int m) {
	static const char* M[12] = { "Jan","Feb","Mar","Apr","May","June","July",
								"Aug","Sep","Oct","Nov","Dec" };
	return (m >= 1 && m <= 12) ? M[m - 1] : std::string();
}
static inline std::string prettyDate_(const std::string& ymd_hms) {
	// Expect "YYYY-MM-DD HH:MM:SS" – be forgiving if not exact
	if (ymd_hms.size() < 10) return ymd_hms;
	int y = 0, m = 0, d = 0;
	try {
		y = std::stoi(ymd_hms.substr(0, 4));
		m = std::stoi(ymd_hms.substr(5, 2));
		d = std::stoi(ymd_hms.substr(8, 2));
	}
	catch (...) { return ymd_hms; }
	std::string mon = monthName_(m);
	if (mon.empty() || d <= 0) return ymd_hms;
	return mon + " " + ordinal_(static_cast<size_t>(d)) + ", " + std::to_string(y);
}

static inline std::string titleFromGameName_(const std::string& gameName) {
	auto pos = gameName.rfind('_');
	if (pos == std::string::npos || pos + 1 >= gameName.size()) return std::string();
	// keep everything after the last '_'
	std::string s = gameName.substr(pos + 1);
	// optional: trim leading/trailing spaces
	auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
	while (!s.empty() && isws((unsigned char)s.front())) s.erase(s.begin());
	while (!s.empty() && isws((unsigned char)s.back()))  s.pop_back();
	return s;
}

// --- Locale-aware numeric date ("6.7.2024" vs "7.6.2024" vs "2024.7.6") ---

enum class DateOrder_ { MDY, DMY, YMD, Unknown };

static DateOrder_ detectDateOrderLocale_() {
	// 22 July 2004 (day > 12 disambiguates MDY vs DMY)
	std::tm tmRef{};
	tmRef.tm_year = 2004 - 1900;
	tmRef.tm_mon = 6;   // July (0-based)
	tmRef.tm_mday = 22;

	std::locale loc;
	try { loc = std::locale(""); }
	catch (...) { /* fall back to "C" */ }

	// SAFER: use the single-char 'x' overload instead of ("%x", "%x"+2)
	std::ostringstream os;
	os.imbue(loc);
	const auto& tp = std::use_facet<std::time_put<char>>(loc);
	std::ostreambuf_iterator<char> it(os);
	tp.put(it, os, os.fill(), &tmRef, 'x');   // formats per locale date (%x)
	const std::string s = os.str();

	// Parse first three integer tokens
	int nums[3] = { -1, -1, -1 };
	int idx = 0;
	for (size_t i = 0; i < s.size() && idx < 3; ) {
		while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
		if (i >= s.size()) break;
		int v = 0;
		while (i < s.size() && std::isdigit((unsigned char)s[i])) {
			v = v * 10 + (s[i] - '0');
			++i;
		}
		nums[idx++] = v;
	}
	if (idx < 3) return DateOrder_::Unknown;

	int a = nums[0], b = nums[1], c = nums[2];
	auto isYear = [](int v) { return v >= 100; }; // four-digit year = definitely year

	// Year-first?
	if (isYear(a)) return DateOrder_::YMD;

	// Year-last? (most locales)
	if (isYear(c)) {
		// decide MDY vs DMY from 7 vs 22
		if (a == 7 && b == 22) return DateOrder_::MDY; // 07/22/2004
		if (a == 22 && b == 7) return DateOrder_::DMY; // 22/07/2004
		// use ranges as fallback
		if (a <= 12 && b >= 13) return DateOrder_::MDY;
		if (a >= 13 && b <= 12) return DateOrder_::DMY;
		return DateOrder_::DMY; // ambiguous -> DMY default
	}

	// Rare: year in the middle (two-digit year)
	if (isYear(b)) {
		if (a == 7 && c == 22) return DateOrder_::MDY;
		if (a == 22 && c == 7) return DateOrder_::DMY;
		return DateOrder_::Unknown;
	}

	// All two-digit components: find "04" (year) position
	if (a == 4) return DateOrder_::YMD;  // 04/07/22
	if (c == 4) {
		if (a == 7 && b == 22) return DateOrder_::MDY;
		if (a == 22 && b == 7)  return DateOrder_::DMY;
		if (a <= 12 && b >= 13) return DateOrder_::MDY;
		if (a >= 13 && b <= 12) return DateOrder_::DMY;
		return DateOrder_::DMY;
	}
	if (b == 4) {
		if (a == 7 && c == 22) return DateOrder_::MDY;
		if (a == 22 && c == 7)  return DateOrder_::DMY;
		return DateOrder_::Unknown;
	}

	return DateOrder_::Unknown;
}
static inline DateOrder_ cachedDateOrder_() {
	static std::once_flag once;
	static DateOrder_ cached = DateOrder_::Unknown;
	std::call_once(once, [] { cached = detectDateOrderLocale_(); });
	return cached;
}

static inline std::string two_(int v) {
	std::ostringstream oss;
	oss << std::setw(2) << std::setfill('0') << v;
	return oss.str();
}

static inline std::string formatDateDotsLocale_(int y, int m, int d) {
	auto ord = cachedDateOrder_();
	switch (ord) {
		case DateOrder_::MDY: return two_(m) + "." + two_(d) + "." + std::to_string(y); // 05.23.2024
		case DateOrder_::DMY: return two_(d) + "." + two_(m) + "." + std::to_string(y); // 23.05.2024
		case DateOrder_::YMD: return std::to_string(y) + "." + two_(m) + "." + two_(d); // 2024.05.23
		default:               return two_(d) + "." + two_(m) + "." + std::to_string(y); // fallback
	}
}

static inline std::string prettyDateNumericDots_(const std::string& ymd_hms) {
	// Expect "YYYY-MM-DD HH:MM:SS" (time part optional); be forgiving.
	if (ymd_hms.size() < 10) return ymd_hms;
	int y = 0, m = 0, d = 0;
	try {
		y = std::stoi(ymd_hms.substr(0, 4));
		m = std::stoi(ymd_hms.substr(5, 2));
		d = std::stoi(ymd_hms.substr(8, 2));
	}
	catch (...) {
		return ymd_hms; // fallback to raw if parsing fails
	}
	return formatDateDotsLocale_(y, m, d);
}


HighScoreData* HiScores::getGlobalHiScoreTable(Item* item) const {
	static thread_local HighScoreData scratch;
	scratch.tables.clear();
	if (!item) return &scratch;

	const std::string idsCsv = item->iscoredId;
	const std::string typesCsv = item->iscoredType;

	const auto ids = splitCSV_(idsCsv);
	if (ids.empty()) return &scratch;

	// Parse per-table sort modes (aligned with ids); repeat last if fewer provided.
	std::vector<SortCfg> sorts;
	{
		const auto typeTokens = splitCSV_(typesCsv);
		if (!typeTokens.empty()) {
			sorts.reserve(typeTokens.size());
			for (const auto& t : typeTokens) sorts.push_back(parseSortAndDp_(t));
		}
		else {
			sorts.push_back(parseSortAndDp_(typesCsv));
		}
	}
	auto cfgOf = [&](int idx) -> const SortCfg& {
		if (idx < (int)sorts.size()) return sorts[idx];
		return sorts.back();
		};

	// Collect pages in the same order as ids
	struct Page { std::string title; std::vector<GlobalRow> rows; };
	std::vector<Page> pages;
	{
		std::shared_lock<std::shared_mutex> lk(globalMutex_);
		pages.reserve(ids.size());
		for (const auto& id : ids) {
			auto it = global_.byId.find(id);
			if (it == global_.byId.end()) continue;
			const GlobalGame& gg = it->second;
			Page p;
			p.rows = gg.rows;
			p.title = titleFromGameName_(gg.gameName); // substring after last '_' (keeps spaces)
			pages.push_back(std::move(p));
		}
	}
	if (pages.empty()) return &scratch;

	// Comparators parameterized by mode
	auto cmpScore = [&](GlobalSort mode, const GlobalRow& a, const GlobalRow& b) {
		double na, nb; bool ha = parseNumber_(a.score, na), hb = parseNumber_(b.score, nb);
		if (ha && hb) return (mode == GlobalSort::ScoreAsc) ? (na < nb) : (na > nb);
		if (ha != hb)  return (mode == GlobalSort::ScoreAsc) ? ha : hb;
		int c = a.score.compare(b.score);
		return (mode == GlobalSort::ScoreAsc) ? (c < 0) : (c > 0);
		};
	auto cmpTime = [&](GlobalSort mode, const GlobalRow& a, const GlobalRow& b) {
		long long ta, tb; bool ha = parseLongLongStrict_(a.score, ta), hb = parseLongLongStrict_(b.score, tb);
		if (ha && hb) return (mode == GlobalSort::TimeAsc) ? (ta < tb) : (ta > tb);
		if (ha != hb) return (mode == GlobalSort::TimeAsc) ? ha : hb;
		int c = a.score.compare(b.score);
		return (mode == GlobalSort::TimeAsc) ? (c < 0) : (c > 0);
		};
	auto cmpMoney = [&](GlobalSort mode, const GlobalRow& a, const GlobalRow& b) {
		long long va, vb; bool ha = parseLongLongStrict_(a.score, va), hb = parseLongLongStrict_(b.score, vb);
		if (ha && hb) return (mode == GlobalSort::MoneyAsc) ? (va < vb) : (va > vb);
		if (ha != hb) return (mode == GlobalSort::MoneyAsc) ? ha : hb;
		int c = a.score.compare(b.score);
		return (mode == GlobalSort::MoneyAsc) ? (c < 0) : (c > 0);
		};
	auto cmpDist = [&](GlobalSort mode, const GlobalRow& a, const GlobalRow& b) {
		long long ca, cb; bool ha = toCanonicalDistance_(mode, a.score, ca), hb = toCanonicalDistance_(mode, b.score, cb);
		const bool asc = (mode == GlobalSort::DistanceCmAsc || mode == GlobalSort::DistanceMAsc ||
			mode == GlobalSort::DistanceKmAsc || mode == GlobalSort::DistanceMilesAsc ||
			mode == GlobalSort::DistanceCmMAsc || mode == GlobalSort::DistanceInAsc ||
			mode == GlobalSort::DistanceFtAsc || mode == GlobalSort::DistanceFtInAsc ||
			mode == GlobalSort::DistanceYdAsc);
		if (ha && hb) return asc ? (ca < cb) : (ca > cb);
		if (ha != hb) return ha;
		int c = a.score.compare(b.score);
		return asc ? (c < 0) : (c > 0);
		};
	auto cmpWeight = [&](GlobalSort mode, const GlobalRow& a, const GlobalRow& b) {
		long long ca, cb; bool ha = toCanonicalWeight_(mode, a.score, ca), hb = toCanonicalWeight_(mode, b.score, cb);
		const bool asc = (mode == GlobalSort::WeightGAsc || mode == GlobalSort::WeightKgAsc || mode == GlobalSort::WeightKgGAsc);
		if (ha && hb) return asc ? (ca < cb) : (ca > cb);
		if (ha != hb) return ha;
		int c = a.score.compare(b.score);
		return asc ? (c < 0) : (c > 0);
		};
	auto cmpScaled = [&](GlobalSort m, const GlobalRow& a, const GlobalRow& b) {
		double va, vb;
		bool ha = getScaledScore_(m, a.score, va);
		bool hb = getScaledScore_(m, b.score, vb);
		const bool asc =
			(m == GlobalSort::DivideBy10Asc || m == GlobalSort::DivideBy100Asc ||
				m == GlobalSort::DivideBy1000Asc || m == GlobalSort::MultiplyBy10Asc ||
				m == GlobalSort::MultiplyBy100Asc || m == GlobalSort::MultiplyBy1000Asc);
		if (ha && hb) return asc ? (va < vb) : (va > vb);
		if (ha != hb) return ha; // numeric beats non-numeric
		int c = a.score.compare(b.score);
		return asc ? (c < 0) : (c > 0);
		};

	// Build each table independently with its own mode
	for (int i = 0; i < (int)pages.size(); ++i) {
		auto& pg = pages[i];
		const SortCfg cfg = cfgOf(i);
		const GlobalSort mode = cfg.mode;


		const bool isTime = (mode == GlobalSort::TimeAsc || mode == GlobalSort::TimeDesc);
		const bool isMoney = (mode == GlobalSort::MoneyAsc || mode == GlobalSort::MoneyDesc);
		const bool isDist =
			mode == GlobalSort::DistanceCmAsc || mode == GlobalSort::DistanceCmDesc ||
			mode == GlobalSort::DistanceMAsc || mode == GlobalSort::DistanceMDesc ||
			mode == GlobalSort::DistanceKmAsc || mode == GlobalSort::DistanceKmDesc ||
			mode == GlobalSort::DistanceMilesAsc || mode == GlobalSort::DistanceMilesDesc ||
			mode == GlobalSort::DistanceCmMAsc || mode == GlobalSort::DistanceCmMDesc ||
			mode == GlobalSort::DistanceInAsc || mode == GlobalSort::DistanceInDesc ||
			mode == GlobalSort::DistanceFtAsc || mode == GlobalSort::DistanceFtDesc ||
			mode == GlobalSort::DistanceFtInAsc || mode == GlobalSort::DistanceFtInDesc ||
			mode == GlobalSort::DistanceYdAsc || mode == GlobalSort::DistanceYdDesc;

		const bool isWeight =
			mode == GlobalSort::WeightGAsc || mode == GlobalSort::WeightGDesc ||
			mode == GlobalSort::WeightKgAsc || mode == GlobalSort::WeightKgDesc ||
			mode == GlobalSort::WeightKgGAsc || mode == GlobalSort::WeightKgGDesc;

		const char* scoreHeader =
			isTime ? "Time" :
			isMoney ? "Cash" :
			isDist ? "Distance" :
			isWeight ? "Weight" : "Score";

		const std::string phName = "-";
		const std::string phDate = "-";
		const std::string phScore = isTime ? "--:--:---" : (isMoney ? "$-" : "-");

		// 1) Best row per PLAYER (dedupe) for *this mode*
		std::unordered_map<std::string, GlobalRow> bestByName;
		bestByName.reserve(pg.rows.size());
		for (const auto& rRaw : pg.rows) {
			GlobalRow r = rRaw;
			r.player = normName_(r.player);
			if (r.player.empty()) continue;
			auto it = bestByName.find(r.player);
			if (it == bestByName.end() || isBetterForMode_(mode, r, it->second)) {
				bestByName[r.player] = std::move(r);
			}
		}
		pg.rows.clear();
		pg.rows.reserve(bestByName.size());
		for (auto& kv : bestByName) pg.rows.push_back(std::move(kv.second));

		// 2) Sort for *this mode*
		switch (mode) {
			case GlobalSort::ScoreAsc:
			case GlobalSort::ScoreDesc:
			std::stable_sort(pg.rows.begin(), pg.rows.end(),
				[&](const GlobalRow& a, const GlobalRow& b) { return cmpScore(mode, a, b); });
			break;
			case GlobalSort::TimeAsc:
			case GlobalSort::TimeDesc:
			std::stable_sort(pg.rows.begin(), pg.rows.end(),
				[&](const GlobalRow& a, const GlobalRow& b) { return cmpTime(mode, a, b); });
			break;
			case GlobalSort::MoneyAsc:
			case GlobalSort::MoneyDesc:
			std::stable_sort(pg.rows.begin(), pg.rows.end(),
				[&](const GlobalRow& a, const GlobalRow& b) { return cmpMoney(mode, a, b); });
			break;

			// NEW scaled score types:
			case GlobalSort::DivideBy10Asc:   case GlobalSort::DivideBy10Desc:
			case GlobalSort::DivideBy100Asc:  case GlobalSort::DivideBy100Desc:
			case GlobalSort::DivideBy1000Asc: case GlobalSort::DivideBy1000Desc:
			case GlobalSort::MultiplyBy10Asc: case GlobalSort::MultiplyBy10Desc:
			case GlobalSort::MultiplyBy100Asc: case GlobalSort::MultiplyBy100Desc:
			case GlobalSort::MultiplyBy1000Asc: case GlobalSort::MultiplyBy1000Desc:
			std::stable_sort(pg.rows.begin(), pg.rows.end(),
				[&](const GlobalRow& a, const GlobalRow& b) { return cmpScaled(mode, a, b); });
			break;

			default:
			if (isDist)
				std::stable_sort(pg.rows.begin(), pg.rows.end(),
					[&](const GlobalRow& a, const GlobalRow& b) { return cmpDist(mode, a, b); });
			else if (isWeight)
				std::stable_sort(pg.rows.begin(), pg.rows.end(),
					[&](const GlobalRow& a, const GlobalRow& b) { return cmpWeight(mode, a, b); });
			else
				std::stable_sort(pg.rows.begin(), pg.rows.end(),
					[&](const GlobalRow& a, const GlobalRow& b) { return cmpScore(mode, a, b); });
			break;
		}

		// 3) Emit top 10 with formatting
		HighScoreTable t;
		t.id = pg.title;                             // show title even for single table
		t.columns = { "Rank", "Name", scoreHeader, "Date" };

		constexpr size_t kRowsPerTable = 10;
		t.rows.reserve(kRowsPerTable);

		size_t rank = 1;
		for (const auto& r : pg.rows) {
			if (rank > kRowsPerTable) break;

			const std::string datePretty = prettyDateNumericDots_(r.date);
			std::string scorePretty;

			if (isTime) {
				long long ms; scorePretty = parseLongLongStrict_(r.score, ms) ? formatMs_(ms) : r.score;
			}
			else if (isMoney) {
				scorePretty = formatMoney_(r.score);
			}
			else if (isDist) {
				long long canon; scorePretty = toCanonicalDistance_(mode, r.score, canon) ? fmtDistance_(mode, canon) : r.score;
			}
			else if (isWeight) {
				long long canon; scorePretty = toCanonicalWeight_(mode, r.score, canon) ? fmtWeight_(mode, canon) : r.score;
			}
			else {
				if (isScaledScoreMode_(mode)) {
					double val;
					if (getScaledScore_(mode, r.score, val)) {
						const int dp = cfg.hasDpOverride ? cfg.dpOverride : -1; // -1 => use default for the mode
						scorePretty = formatScaledScoreStr_(mode, val, dp);
					}
					else {
						scorePretty = formatThousands_(r.score); // fallback
					}
				}
				else {
					scorePretty = formatThousands_(r.score);
				}
			}

			t.rows.push_back({
				ordinal_(rank++),
				r.player.empty() ? phName : r.player,
				scorePretty.empty() ? phScore : scorePretty,
				datePretty.empty() ? phDate : datePretty
				});
		}

		for (; rank <= kRowsPerTable; ++rank) {
			t.rows.push_back({ ordinal_(rank), phName, phScore, phDate });
		}

		t.forceRedraw = true;
		scratch.tables.push_back(std::move(t));
	}

	return &scratch;
}