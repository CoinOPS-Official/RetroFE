#ifdef WIN32
    #include <Windows.h>
#else
    #include <cstdlib>  // For system() on Unix-based systems
    #include <cstring>
#endif

#include "HiScores.h"
#include "../Utility/Utils.h"
#include "../Utility/Log.h"
#include "../Collection/Item.h" 
#include "minizip/unzip.h"
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

#include <numeric>
#include <climits>

#include <nlohmann/json.hpp> // single-header JSON
#include <curl/curl.h>       // libcurl (replace if you have your own HTTP)
using json = nlohmann::json;

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
    } else {
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
    } catch (const rapidxml::parse_error& e) {
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
            } else {
                LOG_ERROR("HiScores", "runHi2Txt failed in the background for game " + gameName);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("HiScores", "Exception in runHi2TxtAsync for game " + gameName + ": " + e.what());
        } catch (...) {
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
            const std::string url = "https://www.iscored.info/api/" + iscoredGameroom_ + "/getAllScores";
            std::string body, err;
            if (!httpGet_(url, body, err)) {
                LOG_WARNING("HiScores", "getAllScores HTTP failed: " + err);
                return;
            }
            std::vector<std::string> changed;
            ingestIScoredAllIncremental_(body, limit, &changed);
            LOG_INFO("HiScores", "Global update: " + std::to_string(changed.size()) + " games changed.");
        }
        catch (const std::exception& e) {
            LOG_ERROR("HiScores", std::string("refreshGlobalAllFromSingleCallAsync exception: ") + e.what());
        }
        catch (...) {
            LOG_ERROR("HiScores", "refreshGlobalAllFromSingleCallAsync: unknown exception");
        }
        }).detach();
}

void HiScores::ingestIScoredAllIncremental_(const std::string& jsonText,
    int capPerGame,
    std::vector<std::string>* changedIds) {
    using nlohmann::json;

    // 1) Parse and group incoming payload by gameId (in a temp map)
    struct TmpGame { std::string name; std::vector<GlobalRow> rows; };
    std::unordered_map<std::string, TmpGame> incoming;

    auto ensure = [&](const std::string& gid, const std::string& gname) -> TmpGame& {
        auto& tg = incoming[gid];
        if (tg.name.empty() && !gname.empty()) tg.name = gname;
        return tg;
        };
    auto pushRow = [&](const std::string& gid, const std::string& gname, const json& s) {
        TmpGame& tg = ensure(gid, gname);
        GlobalRow r;
        r.player = s.value("name", "");
        r.score = s.contains("score") ? j2s(s["score"]) : "";
        r.date = s.value("date", "");
        tg.rows.push_back(std::move(r));
        };

    json j;
    try { j = json::parse(jsonText); }
    catch (const std::exception& e) {
        LOG_ERROR("HiScores", std::string("ingestIScoredAllIncremental_: JSON parse error: ") + e.what());
        return;
    }

    // Shape A: { "games": [ { gameId, gameName, scores:[...] }, ... ] }
    if (j.is_object() && j.contains("games") && j["games"].is_array()) {
        for (const auto& g : j["games"]) {
            const std::string gid = g.contains("gameId") ? j2s(g["gameId"]) : "";
            const std::string gname = g.contains("gameName") ? j2s(g["gameName"]) : "";
            if (gid.empty()) continue;
            if (g.contains("scores") && g["scores"].is_array()) {
                for (const auto& s : g["scores"]) pushRow(gid, gname, s);
            }
        }
    }
    ///// Shape B: { "scores": [ { game, gameName, name, score, date }, ... ] }
    else if (j.is_object() && j.contains("scores") && j["scores"].is_array()) {
        for (const auto& s : j["scores"]) {
            const std::string gid = s.contains("game") ? j2s(s["game"]) : "";
            const std::string gname = s.contains("gameName") ? j2s(s["gameName"]) : "";
            if (gid.empty()) continue;
            pushRow(gid, gname, s);
        }
    }
    // Shape C: top-level array of rows
    else if (j.is_array()) {
        for (const auto& s : j) {
            if (!s.is_object()) continue;
            const std::string gid = s.contains("game") ? j2s(s["game"]) : "";
            const std::string gname = s.contains("gameName") ? j2s(s["gameName"]) : "";
            if (gid.empty()) continue;
            pushRow(gid, gname, s);
        }
    }
    // Shape D: mapping { "<gameId>": [rows...] } or { "<gameId>": { gameName, scores:[...] } }
    else if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string gid = it.key();
            const json& v = it.value();
            if (v.is_array()) {
                for (const auto& s : v) pushRow(gid, "", s);
            }
            else if (v.is_object() && v.contains("scores") && v["scores"].is_array()) {
                const std::string gname = v.contains("gameName") ? j2s(v["gameName"]) : "";
                for (const auto& s : v["scores"]) pushRow(gid, gname, s);
            }
        }
    }
    else {
        LOG_WARNING("HiScores", "ingestIScoredAllIncremental_: Unrecognized JSON shape");
    }

    // Cap rows per game (order as received)
    if (capPerGame > 0) {
        for (auto& kv : incoming) {
            auto& rows = kv.second.rows;
            if (rows.size() > static_cast<size_t>(capPerGame))
                rows.resize(static_cast<size_t>(capPerGame));
        }
    }

    // 2) Diff against current store and update only changed gameIds
    std::vector<std::string> localChanged;
    {
        std::unique_lock<std::shared_mutex> lk(globalMutex_);
        for (auto& kv : incoming) {
            const std::string& gid = kv.first;
            TmpGame& tg = kv.second;

            auto it = global_.byId.find(gid);
            if (it == global_.byId.end()) {
                // New gameId ? insert
                GlobalGame gg;
                gg.gameId = gid;
                gg.gameName = tg.name;
                gg.rows = std::move(tg.rows);
                global_.byId.emplace(gid, std::move(gg));
                localChanged.push_back(gid);
                continue;
            }

            GlobalGame& existing = it->second;
            // Name update if provided
            if (!tg.name.empty() && tg.name != existing.gameName) {
                existing.gameName = tg.name;
                // fall through to row compare; treat as change if rows differ
            }

            // Rows equal (as sets)? Then skip.
            if (rowsEqualAsSets_(existing.rows, tg.rows)) {
                continue;
            }

            // Replace rows
            existing.rows = std::move(tg.rows);
            localChanged.push_back(gid);
        }
        // NOTE: we do NOT prune gameIds missing from incoming (keeps offline data). Add a "prune" flag if you want.
    }

    if (changedIds) *changedIds = std::move(localChanged);
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

    GlobalHiScoreData tmp;  // build off-thread, swap in atomically at the end

    auto ensureGame = [&](const std::string& gid, const std::string& gname) -> GlobalGame& {
        GlobalGame& gg = tmp.byId[gid];
        gg.gameId = gid;
        if (!gname.empty() && gg.gameName.empty()) gg.gameName = gname;
        return gg;
        };

    auto pushRow = [&](const std::string& gid, const std::string& gname,
        const json& s) {
            GlobalGame& gg = ensureGame(gid, gname);
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

    // --- Shape A: { "games": [ { "gameId":..., "gameName":..., "scores":[...] }, ... ] }
    if (j.is_object() && j.contains("games") && j["games"].is_array()) {
        for (const auto& g : j["games"]) {
            const std::string gid = g.contains("gameId") ? j2s(g["gameId"]) : "";
            const std::string gname = g.contains("gameName") ? g["gameName"].get<std::string>() : "";
            if (gid.empty()) continue;
            if (g.contains("scores")) parseScoresArray(gid, gname, g["scores"]);
        }
    }
    // --- Shape B: { "scores": [ { "game":..., "gameName":..., "name":..., "score":..., "date":... }, ... ] }
    else if (j.is_object() && j.contains("scores") && j["scores"].is_array()) {
        for (const auto& s : j["scores"]) {
            const std::string gid = s.contains("game") ? j2s(s["game"]) : "";
            const std::string gname = s.contains("gameName") ? j2s(s["gameName"]) : "";
            if (gid.empty()) continue;
            pushRow(gid, gname, s);
        }
    }
    // --- Shape C: top-level array of rows with { game, gameName, ... }
    else if (j.is_array()) {
        for (const auto& s : j) {
            if (!s.is_object()) continue;
            const std::string gid = s.contains("game") ? j2s(s["game"]) : "";
            const std::string gname = s.contains("gameName") ? j2s(s["gameName"]) : "";
            if (gid.empty()) continue;
            pushRow(gid, gname, s);
        }
    }
    // --- Shape D: object mapping (gameId -> array) or (gameId -> {scores:[...]})
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
    for (auto& kv : tmp.byId) {
        capRows_(kv.second.rows, capPerGame);
    }

    // Atomic install
    {
        std::unique_lock<std::shared_mutex> lk(globalMutex_);
        global_ = std::move(tmp);
    }
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

static inline std::string trim_(std::string s) {
    auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
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

// mm:ss:ms formatter (zero-padded, ms = 3 digits)
static inline std::string formatMs_(long long ms) {
    if (ms < 0) ms = -ms;
    long long totalSec = ms / 1000;
    int msec = (int)(ms % 1000);
    long long minutes = totalSec / 60;
    int seconds = (int)(totalSec % 60);
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << minutes
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

enum class GlobalSort { ScoreDesc, ScoreAsc, TimeAsc, TimeDesc, MoneyDesc, MoneyAsc };

static inline GlobalSort parseSort_(std::string s) {
    s = Utils::toLower(trim_(s));
    if (s == "ascending" || s == "asc") return GlobalSort::ScoreAsc;
    if (s == "timeascending" || s == "time-ascending" || s == "timeasc" || s == "time-asc") return GlobalSort::TimeAsc;
    if (s == "timedescending" || s == "time-descending" || s == "timedesc" || s == "time-desc") return GlobalSort::TimeDesc;
    if (s == "moneyascending" || s == "money-ascending" || s == "moneyasc" || s == "money-asc") return GlobalSort::MoneyAsc;
    if (s == "moneydescending" || s == "money-descending" || s == "moneydesc" || s == "money-desc") return GlobalSort::MoneyDesc;
    return GlobalSort::ScoreDesc; // default
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
static inline bool parseNumber_(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}
static inline std::string modeFromGameName_(const std::string& gameName) {
    // Return suffix after last '_' if present, else full string
    auto pos = gameName.rfind('_');
    if (pos == std::string::npos || pos + 1 >= gameName.size()) return gameName;
    return gameName.substr(pos + 1);
}

HighScoreData* HiScores::getGlobalHiScoreTable(Item* item) const {
    static thread_local HighScoreData scratch;
    scratch.tables.clear();
    if (!item) return &scratch;

    std::string idsCsv = item->iscoredId;             // comma-separated ids
    std::string sortTag = item->iscoredType;      // ascending/descending/time.../money...
    auto sortKind = parseSort_(sortTag);

    auto ids = splitCSV_(idsCsv);
    if (ids.empty()) return &scratch;

    // Snapshot pages for those ids
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
            p.title = (ids.size() > 1) ? modeFromGameName_(gg.gameName) : std::string();
            pages.push_back(std::move(p));
        }
    }
    if (pages.empty()) return &scratch;

    // Sorting lambdas
    auto cmpScore = [sortKind](const GlobalRow& a, const GlobalRow& b) {
        double na, nb;
        bool ha = parseNumber_(a.score, na);
        bool hb = parseNumber_(b.score, nb);
        if (ha && hb) return (sortKind == GlobalSort::ScoreAsc) ? (na < nb) : (na > nb);
        if (ha != hb) return (sortKind == GlobalSort::ScoreAsc) ? ha : hb; // numeric first
        int c = a.score.compare(b.score);
        return (sortKind == GlobalSort::ScoreAsc) ? (c < 0) : (c > 0);
        };
    auto cmpTime = [sortKind](const GlobalRow& a, const GlobalRow& b) {
        long long ta, tb;
        bool ha = parseLongLongStrict_(a.score, ta);
        bool hb = parseLongLongStrict_(b.score, tb);
        if (ha && hb) return (sortKind == GlobalSort::TimeAsc) ? (ta < tb) : (ta > tb);
        if (ha != hb) return (sortKind == GlobalSort::TimeAsc) ? ha : hb; // numeric first
        int c = a.score.compare(b.score);
        return (sortKind == GlobalSort::TimeAsc) ? (c < 0) : (c > 0);
        };
    auto cmpMoney = [sortKind](const GlobalRow& a, const GlobalRow& b) {
        long long va, vb;
        bool ha = parseLongLongStrict_(a.score, va);
        bool hb = parseLongLongStrict_(b.score, vb);
        if (ha && hb) return (sortKind == GlobalSort::MoneyAsc) ? (va < vb) : (va > vb);
        if (ha != hb) return (sortKind == GlobalSort::MoneyAsc) ? ha : hb; // numeric first
        int c = a.score.compare(b.score);
        return (sortKind == GlobalSort::MoneyAsc) ? (c < 0) : (c > 0);
        };

    const bool isTime = (sortKind == GlobalSort::TimeAsc || sortKind == GlobalSort::TimeDesc);
    const bool isMoney = (sortKind == GlobalSort::MoneyAsc || sortKind == GlobalSort::MoneyDesc);
    const char* scoreHeader = isTime ? "Time" : "Score";

    for (auto& pg : pages) {
        switch (sortKind) {
            case GlobalSort::ScoreAsc:
            case GlobalSort::ScoreDesc:
            std::stable_sort(pg.rows.begin(), pg.rows.end(), cmpScore);
            break;
            case GlobalSort::TimeAsc:
            case GlobalSort::TimeDesc:
            std::stable_sort(pg.rows.begin(), pg.rows.end(), cmpTime);
            break;
            case GlobalSort::MoneyAsc:
            case GlobalSort::MoneyDesc:
            std::stable_sort(pg.rows.begin(), pg.rows.end(), cmpMoney);
            break;
        }

        HighScoreTable t;
        t.id = pg.title; // empty if single-id
        t.columns = { "Rank","Name", scoreHeader, "Date" };
        t.rows.reserve(pg.rows.size());

        size_t rank = 1;
        for (const auto& r : pg.rows) {
            std::string datePretty = prettyDate_(r.date);
            std::string scorePretty;

            if (isTime) {
                long long ms;
                scorePretty = parseLongLongStrict_(r.score, ms) ? formatMs_(ms) : r.score;
            }
            else if (isMoney) {
                scorePretty = formatMoney_(r.score);
            }
            else {
                scorePretty = formatThousands_(r.score);
            }

            t.rows.push_back({ ordinal_(rank++), r.player, scorePretty, datePretty });
        }

        t.forceRedraw = true;
        scratch.tables.push_back(std::move(t));
    }

    return &scratch;
}