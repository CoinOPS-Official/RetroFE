/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef WIN32

#include "WindowsProcessManager.h"

#include <fstream>
#include <filesystem>
#include <vector>
#include <chrono>
#include <cstdlib>   // getenv
#include <algorithm> // std::replace
#include <cctype>    // std::isdigit
#include <set>

#include <windows.h>
#include <Psapi.h>   // GetModuleFileNameExA / QueryFullProcessImageNameA
#include <Tlhelp32.h>
#include <winreg.h>

#include <SDL3/SDL.h>

#include "../../../SDL.h"                 // SDL::getWindow
#include "../../../Utility/Log.h"
#include "../../../Utility/Utils.h"       // Utils::toLower
#include "../../../Database/Configuration.h"

namespace {
    // --- General helpers ----------------------------------------------------

    std::string getExeNameFromPath(const std::string& path) {
        auto pos = path.find_last_of("/\\");
        if (pos != std::string::npos) {
            return path.substr(pos + 1);
        }
        return path;
    }

    static bool isMameExeName(const std::string& exeName) {
        std::string n = Utils::toLower(exeName);
        return (n.rfind("mame", 0) == 0); // starts with "mame"
    }

    // --- Simple string helpers ----------------------------------------------

    static std::string trimQuotes(const std::string& s) {
        size_t start = 0;
        size_t end = s.size();
        while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '"')) ++start;
        while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '"')) --end;
        return s.substr(start, end - start);
    }

    static std::string readRegString(HKEY root, const char* subkey, const char* valueName) {
        HKEY hKey;
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            return {};
        }

        char  buf[1024];
        DWORD type = 0;
        DWORD size = sizeof(buf);
        std::string result;

        if (RegQueryValueExA(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            result.assign(buf, size);
            while (!result.empty() && result.back() == '\0') {
                result.pop_back();
            }
        }

        RegCloseKey(hKey);
        return result;
    }

    // --- Steam resolution helpers -------------------------------------------

    // Global (per-process) Steam game root for the *current* launch.
    static std::string g_steamGameRoot;
    static bool        g_hasSteamGameRoot = false;

    // Global (per-process) Epic game root for the *current* launch.
    static std::string g_epicGameRoot;
    static bool        g_hasEpicGameRoot = false;

    // Read a .url InternetShortcut and return the URL= value (steam://...).
    static std::string readSteamUrlFromInternetShortcut(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) return {};

        std::string line;
        while (std::getline(in, line)) {
            // crude parse: URL=steam://rungameid/123456
            if (line.rfind("URL=", 0) == 0 ||
                line.rfind("Url=", 0) == 0 ||
                line.rfind("url=", 0) == 0)
            {
                return line.substr(4);
            }
        }
        return {};
    }

    static std::string getSteamInstallPath() {
        // Try HKCU first
        std::string path = readRegString(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
        if (!path.empty()) return path;

        // HKLM 32-bit
        path = readRegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath");
        if (!path.empty()) return path;

        // HKLM 32-bit-on-64
        path = readRegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Wow6432Node\\Valve\\Steam", "InstallPath");
        if (!path.empty()) return path;

        // Fallback: protocol handler
        std::string command = readRegString(HKEY_CLASSES_ROOT, "steam\\Shell\\Open\\Command", "");
        if (!command.empty()) {
            auto firstQuote = command.find('"');
            if (firstQuote != std::string::npos) {
                auto secondQuote = command.find('"', firstQuote + 1);
                if (secondQuote != std::string::npos) {
                    std::string exePath = command.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                    std::filesystem::path p(exePath);
                    return p.parent_path().string();
                }
            }
        }

        return {};
    }

    static std::vector<std::filesystem::path> getSteamLibraries() {
        std::vector<std::filesystem::path> libs;

        std::string steamRoot = getSteamInstallPath();
        if (steamRoot.empty()) {
            LOG_WARNING("ProcessManager", "Steam install path not found in registry.");
            return libs;
        }

        std::filesystem::path root(steamRoot);
        if (std::filesystem::exists(root)) {
            libs.push_back(root); // default library
            LOG_INFO("ProcessManager", "Steam default library: " + root.string());
        }

        std::filesystem::path libFile = root / "config" / "libraryfolders.vdf";
        std::ifstream in(libFile);
        if (!in.is_open()) {
            LOG_WARNING("ProcessManager",
                "Could not open libraryfolders.vdf at: " + libFile.string());
            return libs;
        }

        LOG_INFO("ProcessManager",
            "Parsing Steam libraryfolders from: " + libFile.string());

        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find("\"path\"");
            if (pos == std::string::npos) continue;

            // Example:
            //   "path"        "E:\\SteamLibrary"
            //
            // pos -> index of first quote before p in "path"
            // We want the *value* after "path", which is in the next pair of quotes.
            auto firstQuote = line.find('"', pos + 6); // opening quote of the path
            if (firstQuote == std::string::npos) continue;

            auto secondQuote = line.find('"', firstQuote + 1);
            if (secondQuote == std::string::npos) continue;

            std::string rawPath = line.substr(firstQuote + 1,
                secondQuote - firstQuote - 1);
            rawPath = trimQuotes(rawPath);
            if (rawPath.empty()) continue;

            std::filesystem::path p(rawPath);
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(p, ec);
            if (ec) {
                if (std::filesystem::exists(p)) {
                    libs.push_back(p);
                    LOG_INFO("ProcessManager",
                        "Steam library (raw): " + p.string());
                }
            }
            else {
                if (std::filesystem::exists(canon)) {
                    libs.push_back(canon);
                    LOG_INFO("ProcessManager",
                        "Steam library: " + canon.string());
                }
            }
        }

        return libs;
    }

    static std::string resolveSteamGameRoot(const std::string& appId) {
        auto libs = getSteamLibraries();
        if (libs.empty()) return {};

        LOG_INFO("ProcessManager", "resolveSteamGameRoot: trying to locate appmanifest_" + appId + ".acf");

        for (const auto& libRoot : libs) {
            std::filesystem::path manifest = libRoot / "steamapps" / ("appmanifest_" + appId + ".acf");

            std::error_code existsEc;
            bool exists = std::filesystem::exists(manifest, existsEc);
            LOG_INFO("ProcessManager",
                "Checking manifest candidate: " + manifest.string() +
                " (exists=" + std::string(exists ? "true" : "false") + ")");
            if (!exists) continue;

            LOG_INFO("ProcessManager", "Found manifest for app " + appId + " at: " + manifest.string());

            std::ifstream in(manifest);
            if (!in.is_open()) continue;

            std::string line;
            std::string installDirName;

            while (std::getline(in, line)) {
                auto pos = line.find("\"installdir\"");
                if (pos == std::string::npos) continue;

                // We�re now somewhere on:  ..."installdir"   "Game Folder"
                // Move to *after* the closing quote of "installdir"
                const size_t keyLen = std::strlen("\"installdir\"");
                size_t searchFrom = pos + keyLen;

                // First quote after the key: opening quote of the value
                auto valueQuote1 = line.find('"', searchFrom);
                if (valueQuote1 == std::string::npos) continue;

                // Second quote: closing quote of the value
                auto valueQuote2 = line.find('"', valueQuote1 + 1);
                if (valueQuote2 == std::string::npos) continue;

                installDirName = line.substr(valueQuote1 + 1, valueQuote2 - valueQuote1 - 1);
                installDirName = trimQuotes(installDirName);
                break;
            }

            if (installDirName.empty()) {
                LOG_WARNING("ProcessManager",
                    "resolveSteamGameRoot: no installdir entry found in " + manifest.string());
                continue;
            }

            std::filesystem::path gameRoot = libRoot / "steamapps" / "common" / installDirName;
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(gameRoot, ec);
            std::string finalPath = ec ? gameRoot.string() : canon.string();

            LOG_INFO("ProcessManager",
                "resolveSteamGameRoot: resolved game root to: " + finalPath);
            return finalPath;
        }

        LOG_WARNING("ProcessManager",
            "resolveSteamGameRoot: appmanifest_" + appId + ".acf not found in any Steam library.");
        return {};
    }

    static std::string extractSteamAppIdFromString(const std::string& s) {
        auto grabDigits = [](const std::string& str, size_t pos) -> std::string {
            while (pos < str.size() && str[pos] == ' ') ++pos;
            size_t start = pos;
            while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos]))) ++pos;
            if (pos > start) {
                return str.substr(start, pos - start);
            }
            return {};
            };

        std::string lower = Utils::toLower(s);

        // -applaunch NNN
        {
            const std::string key = "-applaunch";
            auto pos = lower.find(key);
            if (pos != std::string::npos) {
                std::string id = grabDigits(s, pos + key.size());
                if (!id.empty()) return id;
            }
        }

        // steam://rungameid/NNN
        {
            const std::string key = "steam://rungameid/";
            auto pos = lower.find(key);
            if (pos != std::string::npos) {
                size_t start = pos + key.size();
                size_t end = start;
                while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
                if (end > start) {
                    return s.substr(start, end - start);
                }
            }
        }

        // Optional: custom marker --steam-appid=NNN
        {
            const std::string key = "--steam-appid=";
            auto pos = lower.find(key);
            if (pos != std::string::npos) {
                size_t start = pos + key.size();
                size_t end = start;
                while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
                if (end > start) {
                    return s.substr(start, end - start);
                }
            }
        }

        return {};
    }

    // Check if a PID's EXE path lives under a given root folder.
    static bool isProcessInFolder(DWORD pid, const std::string& root) {
        if (root.empty()) return false;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return false;

        char  buffer[MAX_PATH];
        DWORD size = MAX_PATH;
        bool  result = false;

        if (QueryFullProcessImageNameA(hProc, 0, buffer, &size)) {
            std::string exePath(buffer, size);
            std::string rootNorm = Utils::toLower(root);
            std::string exeNorm = Utils::toLower(exePath);

            std::replace(rootNorm.begin(), rootNorm.end(), '/', '\\');
            std::replace(exeNorm.begin(), exeNorm.end(), '/', '\\');

            if (exeNorm.size() >= rootNorm.size() &&
                exeNorm.compare(0, rootNorm.size(), rootNorm) == 0 &&
                (exeNorm.size() == rootNorm.size() || exeNorm[rootNorm.size()] == '\\'))
            {
                result = true;
            }
        }

        CloseHandle(hProc);
        return result;
    }

    // Scan processes, return first PID whose EXE lives under root (or 0 if none).
    static DWORD findProcessInFolder(const std::string& root) {
        if (root.empty()) return 0;

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32 pe32{};
        pe32.dwSize = sizeof(PROCESSENTRY32);

        DWORD foundPid = 0;
        if (Process32First(hSnap, &pe32)) {
            DWORD selfPid = GetCurrentProcessId();
            do {
                DWORD pid = pe32.th32ProcessID;
                if (pid == 0 || pid == selfPid) continue;
                if (isProcessInFolder(pid, root)) {
                    foundPid = pid;
                    break;
                }
            } while (Process32Next(hSnap, &pe32));
        }

        CloseHandle(hSnap);
        return foundPid;
    }

    static std::string urlDecode(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size()) {
                char hex[3] = { in[i + 1], in[i + 2], '\0' };
                char* end = nullptr;
                long v = std::strtol(hex, &end, 16);
                if (end != hex) {
                    out.push_back(static_cast<char>(v));
                    i += 2;
                    continue;
                }
            }
            out.push_back(in[i]);
        }
        return out;
    }

    static std::string extractEpicAppNameFromString(const std::string& s) {
        std::string lower = Utils::toLower(s);
        const std::string key = "com.epicgames.launcher://apps/";
        auto pos = lower.find(key);
        if (pos == std::string::npos) {
            return {};
        }

        // Grab everything from after "apps/" up to '?' / whitespace
        size_t start = pos + key.size();
        size_t end = start;
        while (end < s.size() &&
            s[end] != '?' &&
            s[end] != '&' &&
            !std::isspace(static_cast<unsigned char>(s[end])))
        {
            ++end;
        }

        if (end <= start) return {};

        // This will be something like:
        //   "0ea70e...%3A8865...%3A5b60..."
        std::string encodedId = s.substr(start, end - start);

        // Decode percent-escapes -> "namespace:catalogId:appName"
        std::string decodedId = urlDecode(encodedId);

        // Epic uses "namespace:catalogItemId:appName". Manifest AppName is only the last part.
        auto colonPos = decodedId.rfind(':');
        std::string appName;
        if (colonPos != std::string::npos && colonPos + 1 < decodedId.size()) {
            appName = decodedId.substr(colonPos + 1);
        }
        else {
            appName = decodedId;
        }

        // Optional: debug logging to verify
        LOG_INFO("ProcessManager",
            "Epic URL id segment decoded as \"" + decodedId +
            "\", using AppName=\"" + appName + "\"");

        return appName;
    }

    static std::filesystem::path getEpicManifestsDir() {
        char* programData = std::getenv("ProgramData");
        std::filesystem::path base;

        if (programData && *programData) {
            base = std::filesystem::path(programData);
        }
        else {
            // Fallback to typical path if env var is missing
            base = std::filesystem::path("C:\\ProgramData");
        }

        return base / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
    }

    static std::string resolveEpicGameRoot(const std::string& appName) {
        if (appName.empty()) return {};

        auto manifestsDir = getEpicManifestsDir();
        std::error_code ec;
        if (!std::filesystem::exists(manifestsDir, ec) || !std::filesystem::is_directory(manifestsDir, ec)) {
            LOG_WARNING("ProcessManager",
                "Epic manifests directory not found: " + manifestsDir.string());
            return {};
        }

        std::string appNameLower = Utils::toLower(appName);
        LOG_INFO("ProcessManager", "resolveEpicGameRoot: looking for AppName=" + appName);

        for (const auto& entry : std::filesystem::directory_iterator(manifestsDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;

            std::ifstream in(entry.path());
            if (!in.is_open()) continue;

            std::string line;
            std::string manifestAppName;
            std::string installLocation;
            std::string manifestLocation;

            while (std::getline(in, line)) {
                // "AppName": "5b60142e120c4f2d88027595c21d4a04"
                {
                    auto appPos = line.find("\"AppName\"");
                    if (appPos != std::string::npos) {
                        const size_t keyLen = std::strlen("\"AppName\"");
                        size_t searchFrom = appPos + keyLen;
                        auto q1 = line.find('"', searchFrom);
                        if (q1 != std::string::npos) {
                            auto q2 = line.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                manifestAppName = line.substr(q1 + 1, q2 - q1 - 1);
                                manifestAppName = trimQuotes(manifestAppName);
                            }
                        }
                    }
                }

                // "InstallLocation": "E:\\Epic Games\\DOOM64"
                {
                    auto locPos = line.find("\"InstallLocation\"");
                    if (locPos != std::string::npos) {
                        const size_t keyLen = std::strlen("\"InstallLocation\"");
                        size_t searchFrom = locPos + keyLen;
                        auto q1 = line.find('"', searchFrom);
                        if (q1 != std::string::npos) {
                            auto q2 = line.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                installLocation = line.substr(q1 + 1, q2 - q1 - 1);
                                installLocation = trimQuotes(installLocation);
                            }
                        }
                    }
                }

                // "ManifestLocation": "E:\\Epic Games\\DOOM64/.egstore"
                {
                    auto mlPos = line.find("\"ManifestLocation\"");
                    if (mlPos != std::string::npos) {
                        const size_t keyLen = std::strlen("\"ManifestLocation\"");
                        size_t searchFrom = mlPos + keyLen;
                        auto q1 = line.find('"', searchFrom);
                        if (q1 != std::string::npos) {
                            auto q2 = line.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                manifestLocation = line.substr(q1 + 1, q2 - q1 - 1);
                                manifestLocation = trimQuotes(manifestLocation);
                            }
                        }
                    }
                }
            }

            if (!manifestAppName.empty()) {
                std::string mLower = Utils::toLower(manifestAppName);
                if (mLower == appNameLower) {
                    std::filesystem::path rootPath;

                    if (!installLocation.empty()) {
                        rootPath = std::filesystem::path(installLocation);
                    }
                    else if (!manifestLocation.empty()) {
                        // ManifestLocation is usually "<GameDir>/.egstore"
                        std::filesystem::path ml(manifestLocation);
                        rootPath = ml.parent_path(); // E:\Epic Games\DOOM64
                    }
                    else {
                        LOG_WARNING("ProcessManager",
                            "Epic manifest " + entry.path().string() +
                            " matched AppName=" + manifestAppName +
                            " but has no InstallLocation/ManifestLocation.");
                        continue;
                    }

                    auto canon = std::filesystem::weakly_canonical(rootPath, ec);
                    std::string finalPath = ec ? rootPath.string() : canon.string();

                    LOG_INFO("ProcessManager",
                        "resolveEpicGameRoot: matched AppName=" + manifestAppName +
                        " -> game root: " + finalPath +
                        " (manifest: " + entry.path().string() + ")");

                    return finalPath;
                }
            }
        }

        LOG_WARNING("ProcessManager",
            "resolveEpicGameRoot: no manifest matched AppName=" + appName);
        return {};
    }


} // anonymous namespace

// -----------------------------------------------------------------------------
// Window / graceful-shutdown helpers
// -----------------------------------------------------------------------------

struct HwndCollectContext {
    DWORD pid;
    std::vector<HWND>* out;
};

static BOOL CALLBACK EnumWindowsForPidProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<HwndCollectContext*>(lParam);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != ctx->pid) {
        return TRUE; // not our process, skip
    }

    // 1) Must be visible, otherwise we ignore it
    if (!IsWindowVisible(hwnd))
        return TRUE;

    // 2) Skip the desktop and taskbar � they belong to other processes,
    //    but we keep these checks as a safety guard anyway.
    char className[256] = { 0 };
    GetClassNameA(hwnd, className, sizeof(className));
    if (strcmp(className, "Progman") == 0 ||
        strcmp(className, "Shell_TrayWnd") == 0) {
        return TRUE;
    }

    // 3) Must have a non-zero rectangle (actual size on screen)
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc))
        return TRUE;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0)
        return TRUE;

    // If it made it this far, we treat it as a usable window.
    ctx->out->push_back(hwnd);
    return TRUE;
}

static void collectWindowsForPid(DWORD pid, std::vector<HWND>& out) {
    out.clear();
    HwndCollectContext ctx{ pid, &out };
    EnumWindows(EnumWindowsForPidProc, reinterpret_cast<LPARAM>(&ctx));
}

// Politely ask each window to close (fast/non-blocking first; small bounded fallback)
static void sendCloseToWindows(const std::vector<HWND>& windows) {
    for (HWND h : windows) {
        if (!IsWindow(h)) {
            continue;
        }

        char title[512] = {};
        char className[256] = {};

        GetWindowTextA(h, title, sizeof(title));
        GetClassNameA(h, className, sizeof(className));

        LOG_INFO("ProcessManager",
            std::string("Posting SC_CLOSE to hwnd=") +
            std::to_string(reinterpret_cast<uintptr_t>(h)) +
            " class=\"" + className +
            "\" title=\"" + title + "\"");

        PostMessage(h, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
}

static DWORD getJobActiveProcessCount(HANDLE hJob) {
    if (!hJob) {
        return MAXDWORD;
    }

    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(
        hJob,
        JobObjectBasicAccountingInformation,
        &info,
        sizeof(info),
        nullptr))
    {
        return MAXDWORD;
    }

    return info.ActiveProcesses;
}

static bool waitForJobEmptyBounded(HANDLE hJob, DWORD waitMsTotal) {
    if (!hJob) {
        return false;
    }

    const DWORD sliceMs = 100;
    DWORD       waitedMs = 0;
    DWORD       lastActive = MAXDWORD;

    while (waitedMs < waitMsTotal) {
        DWORD active = getJobActiveProcessCount(hJob);

        if (active == 0) {
            LOG_INFO("ProcessManager", "Job is empty.");
            return true;
        }

        if (active != MAXDWORD && active != lastActive) {
            LOG_INFO("ProcessManager",
                "Waiting for job to empty; active process count = " +
                std::to_string(active));
            lastActive = active;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, sliceMs, QS_ALLINPUT);

        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }

        waitedMs += sliceMs;
    }

    DWORD active = getJobActiveProcessCount(hJob);
    if (active != MAXDWORD) {
        LOG_WARNING("ProcessManager",
            "Timed out waiting for job to empty; active process count = " +
            std::to_string(active));
    }
    else {
        LOG_WARNING("ProcessManager",
            "Timed out waiting for job to empty; active process count unavailable.");
    }

    return false;
}

static bool getJobProcessIds(HANDLE hJob, std::vector<DWORD>& outPids) {
    outPids.clear();

    if (!hJob) {
        return false;
    }

    JOBOBJECT_BASIC_PROCESS_ID_LIST header{};
    DWORD bytes = 0;

    QueryInformationJobObject(
        hJob,
        JobObjectBasicProcessIdList,
        &header,
        sizeof(header),
        &bytes);

    if (bytes == 0) {
        return false;
    }

    std::vector<BYTE> buffer(bytes);
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());

    if (!QueryInformationJobObject(
        hJob,
        JobObjectBasicProcessIdList,
        list,
        bytes,
        &bytes))
    {
        return false;
    }

    outPids.reserve(list->NumberOfProcessIdsInList);

    for (ULONG i = 0; i < list->NumberOfProcessIdsInList; ++i) {
        outPids.push_back(static_cast<DWORD>(list->ProcessIdList[i]));
    }

    return true;
}

static bool forceForegroundForPid(DWORD pid) {
    if (!pid) return false;

    std::vector<HWND> hwnds;
    collectWindowsForPid(pid, hwnds);
    if (hwnds.empty()) {
        // Quiet failure � happens often while the game is still starting
        return false;
    }

    // Pick the "largest" window as the most likely main game window
    HWND bestHwnd = nullptr;
    long bestArea = -1;

    for (HWND h : hwnds) {
        RECT rc{};
        if (!GetWindowRect(h, &rc)) continue;
        long w = rc.right - rc.left;
        long hgt = rc.bottom - rc.top;
        long area = (w > 0 && hgt > 0) ? (w * hgt) : -1;
        if (area > bestArea) {
            bestArea = area;
            bestHwnd = h;
        }
    }

    if (!bestHwnd) {
        return false;
    }

    AllowSetForegroundWindow(pid);
    ShowWindow(bestHwnd, SW_SHOWNORMAL);
    SetForegroundWindow(bestHwnd);

    LOG_INFO("ProcessManager",
        "forceForegroundForPid: brought a window for PID " +
        std::to_string(pid) + " to foreground.");
    return true;
}

static bool waitForHandleExitBounded(HANDLE h, DWORD waitMsTotal) {
    if (!h) return false;

    const DWORD slice = 100;
    DWORD       waited = 0;

    while (waited < waitMsTotal) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            return true;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
        waited += slice;
    }
    return false;
}

// Best-effort, bounded wait for a single PID to exit
static bool waitForPidExitBounded(DWORD pid, DWORD waitMsTotal) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;

    const DWORD slice = 100;
    DWORD       waited = 0;

    while (waited < waitMsTotal) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            return true;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
        waited += slice;
    }

    CloseHandle(h);
    return false;
}

// Graceful ask for a single PID
bool WindowsProcessManager::requestGracefulShutdownForPid(DWORD pid, DWORD waitMsTotal) {
    std::vector<HWND> hwnds;
    collectWindowsForPid(pid, hwnds);

    if (hwnds.empty()) {
        return false;
    }

    LOG_INFO("ProcessManager",
        "Sending close to " + std::to_string(hwnds.size()) +
        " window(s) for PID " + std::to_string(pid));

    sendCloseToWindows(hwnds);
    return waitForPidExitBounded(pid, waitMsTotal);
}

// Graceful ask for all processes in our Job.
bool WindowsProcessManager::requestGracefulShutdownForJob(DWORD waitMsTotal) {
    if (!hJob_) {
        return false;
    }

    const DWORD sliceMs = 100;
    DWORD waitedMs = 0;
    DWORD lastActive = MAXDWORD;

    std::set<DWORD> pidsAlreadyClosed;
    std::set<HWND>  hwndsAlreadyClosed;

    while (waitedMs < waitMsTotal) {
        DWORD active = getJobActiveProcessCount(hJob_);

        if (active == 0) {
            LOG_INFO("ProcessManager", "Graceful job shutdown succeeded; all job processes exited.");
            return true;
        }

        if (active != MAXDWORD && active != lastActive) {
            LOG_INFO("ProcessManager",
                "Waiting for job to empty; active process count = " +
                std::to_string(active));
            lastActive = active;
        }

        std::vector<DWORD> pids;
        if (getJobProcessIds(hJob_, pids)) {
            for (DWORD pid : pids) {
                std::vector<HWND> hwnds;
                collectWindowsForPid(pid, hwnds);

                if (hwnds.empty()) {
                    if (pidsAlreadyClosed.insert(pid).second) {
                        LOG_DEBUG("ProcessManager",
                            "Job member PID " + std::to_string(pid) +
                            " has no visible top-level windows.");
                    }
                    continue;
                }

                std::vector<HWND> newHwnds;
                for (HWND hwnd : hwnds) {
                    if (hwndsAlreadyClosed.insert(hwnd).second) {
                        newHwnds.push_back(hwnd);
                    }
                }

                if (!newHwnds.empty()) {
                    LOG_INFO("ProcessManager",
                        "Requesting close for job member PID " +
                        std::to_string(pid) +
                        " (" + std::to_string(newHwnds.size()) +
                        " new window(s))");

                    sendCloseToWindows(newHwnds);
                }
            }
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, sliceMs, QS_ALLINPUT);

        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }

        waitedMs += sliceMs;
    }

    DWORD active = getJobActiveProcessCount(hJob_);
    if (active != MAXDWORD) {
        LOG_WARNING("ProcessManager",
            "Graceful job shutdown timed out; active process count = " +
            std::to_string(active));
    }
    else {
        LOG_WARNING("ProcessManager",
            "Graceful job shutdown timed out; active process count unavailable.");
    }

    return false;
}

// -----------------------------------------------------------------------------
// WindowsProcessManager
// -----------------------------------------------------------------------------

WindowsProcessManager::WindowsProcessManager() {
    LOG_INFO("ProcessManager", "WindowsProcessManager created.");

    bool pendingForeground_ = false;
    std::chrono::steady_clock::time_point foregroundDeadline_;

    hRetroFEWindow_ = nullptr;
    if (!SDL_IsMainThread()) {
        LOG_ERROR("ProcessManager", "Window lookup must run on the SDL main thread.");
        return;
    }
    SDL_Window* mainWindow = SDL::getWindow(0);
    if (mainWindow) {
        hRetroFEWindow_ = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(mainWindow), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    }
}

WindowsProcessManager::~WindowsProcessManager() {
    cleanupHandles();
}

void WindowsProcessManager::cleanupHandles() {
    if (hProcess_ != nullptr) {
        CloseHandle(hProcess_);
        hProcess_ = nullptr;
    }
    if (hJob_ != nullptr) {
        CloseHandle(hJob_);
        hJob_ = nullptr;
    }
    jobAssigned_ = false;
    processId_ = 0;
    executableName_.clear();
    workingDirectory_.clear();

    // reset Steam/Epic tracking for next launch
    g_steamGameRoot.clear();
    g_hasSteamGameRoot = false;
    g_epicGameRoot.clear();
    g_hasEpicGameRoot = false;
}

bool WindowsProcessManager::simpleLaunch(const std::string& executable,
    const std::string& args,
    const std::string& currentDirectory) {
    std::string ext = Utils::toLower(std::filesystem::path(executable).extension().string());
    bool        isBatch = (ext == ".bat" || ext == ".cmd");

    std::string commandLine;
    std::string workDir = currentDirectory;

    STARTUPINFOA        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (isBatch) {
        char* comspec = std::getenv("COMSPEC");
        std::string shell = comspec ? comspec : "C:\\Windows\\System32\\cmd.exe";
        commandLine = "\"" + shell + "\" /C \"\"" + executable + "\"";
        if (!args.empty()) commandLine += " " + args;
        commandLine += "\"";
    }
    else {
        commandLine = "\"" + executable + "\"";
        if (!args.empty()) commandLine += " " + args;
    }

    if (!CreateProcessA(
        nullptr,
        &commandLine[0],
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
        &si,
        &pi))
    {
        LOG_ERROR("ProcessManager", "simpleLaunch failed: " + commandLine +
            " (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool WindowsProcessManager::launch(const std::string& executable,
    const std::string& args,
    const std::string& currentDirectory) {
    cleanupHandles();

    // Resolve executable path to something usable on disk first.
    std::filesystem::path exePath(executable);
    if (!exePath.is_absolute()) exePath = std::filesystem::absolute(exePath);

    std::string exePathStr = exePath.string();
    std::string extension = Utils::toLower(exePath.extension().string());

    //
 // --- STEP 0: Store-specific detection (Steam / Epic, supports .url shortcuts) ---
 //
    {
        std::string steamSpec;
        std::string epicSpec;

        if (extension == ".url") {
            // .url = InternetShortcut; we need to read the file and find "URL=..."
            std::ifstream urlFile(exePathStr);
            if (urlFile.is_open()) {
                std::string line;
                while (std::getline(urlFile, line)) {
                    auto pos = line.find("URL=");
                    if (pos != std::string::npos) {
                        std::string url = line.substr(pos + 4); // after "URL="
                        url = trimQuotes(url);
                        if (!url.empty()) {
                            std::string lowerUrl = Utils::toLower(url);
                            if (lowerUrl.rfind("steam://", 0) == 0) {
                                steamSpec = url;
                                LOG_INFO("ProcessManager",
                                    "Parsed .url as Steam URL: " + url);
                            }
                            else if (lowerUrl.rfind("com.epicgames.launcher://", 0) == 0) {
                                epicSpec = url;
                                LOG_INFO("ProcessManager",
                                    "Parsed .url as Epic URL: " + url);
                            }
                            else {
                                LOG_INFO("ProcessManager",
                                    "Parsed .url but URL scheme is not Steam or Epic: " + url);
                            }
                        }
                        break;
                    }
                }
                urlFile.close();
            }
            else {
                LOG_WARNING("ProcessManager",
                    "Could not open .url file: " + exePathStr);
            }
        }
        else {
            // Non-.url: fall back to the existing behavior of inspecting exe+args
            std::string combined = executable;
            if (!args.empty()) {
                combined += " ";
                combined += args;
            }
            // We can reuse the same combined string for both detectors.
            steamSpec = combined;
            epicSpec = combined;
        }

        // --- Steam detection ---
        if (!steamSpec.empty()) {
            std::string appId = extractSteamAppIdFromString(steamSpec);
            if (!appId.empty()) {
                std::string root = resolveSteamGameRoot(appId);
                if (!root.empty()) {
                    g_steamGameRoot = root;
                    g_hasSteamGameRoot = true;
                    LOG_INFO("ProcessManager",
                        "Detected Steam appid " + appId +
                        ", resolved game root: " + g_steamGameRoot);
                }
                else {
                    LOG_WARNING("ProcessManager",
                        "Steam appid " + appId +
                        " detected but appmanifest not found; falling back to normal behavior.");
                }
            }
        }

        // --- Epic detection ---
        if (!epicSpec.empty()) {
            std::string epicAppName = extractEpicAppNameFromString(epicSpec);
            if (!epicAppName.empty()) {
                std::string root = resolveEpicGameRoot(epicAppName);
                if (!root.empty()) {
                    g_epicGameRoot = root;
                    g_hasEpicGameRoot = true;
                    LOG_INFO("ProcessManager",
                        "Detected Epic AppName " + epicAppName +
                        ", resolved game root: " + g_epicGameRoot);
                }
                else {
                    LOG_WARNING("ProcessManager",
                        "Epic AppName " + epicAppName +
                        " detected but no matching manifest / install location; falling back to normal behavior.");
                }
            }
        }
    }


    // Current directory
    std::filesystem::path currDir(currentDirectory);
    if (!currDir.is_absolute()) currDir = std::filesystem::absolute(currDir);
    std::string currDirStr = currDir.string();

    executableName_ = getExeNameFromPath(exePathStr);
    workingDirectory_ = currDirStr;

    LOG_INFO("ProcessManager", "Attempting to launch: " + exePathStr);
    if (!args.empty()) LOG_INFO("ProcessManager", "           Arguments: " + args);
    LOG_INFO("ProcessManager", "     Working directory: " + currDirStr);

    // Create/config job
    hJob_ = CreateJobObject(NULL, NULL);
    if (hJob_ == NULL) {
        LOG_ERROR("ProcessManager", "Failed to create Job Object. Error: " + std::to_string(GetLastError()));
    }
    else {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            LOG_WARNING("ProcessManager", "Failed to set Job Object limits. Error: " + std::to_string(GetLastError()));
        }
    }

    bool isExe = (extension == ".exe");
    bool isBat = (extension == ".bat" || extension == ".cmd");
    bool launchCommandSent = false;

    if (isExe || isBat) {
        STARTUPINFOA        startupInfo{};
        PROCESS_INFORMATION processInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.wShowWindow = SW_SHOWDEFAULT;

        std::string commandLine;

        if (isExe) {
            commandLine = "\"" + exePathStr + "\"";
            if (!args.empty()) commandLine += " " + args;
        }
        else {
            const char* comspec = std::getenv("COMSPEC");
            std::string shell = comspec ? comspec : "C:\\Windows\\System32\\cmd.exe";
            commandLine = "\"" + shell + "\" /C \"\"" + exePathStr + "\"";
            if (!args.empty()) commandLine += " " + args;
            commandLine += "\"";
        }

        DWORD flags = CREATE_NO_WINDOW;
        if (CreateProcessA(nullptr, &commandLine[0], nullptr, nullptr,
            TRUE, flags, nullptr, currDirStr.c_str(),
            &startupInfo, &processInfo))
        {
            launchCommandSent = true;
            hProcess_ = processInfo.hProcess;
            processId_ = processInfo.dwProcessId;

            if (hJob_ && AssignProcessToJobObject(hJob_, hProcess_)) {
                jobAssigned_ = true;
                LOG_INFO("ProcessManager", "Process assigned to Job Object.");
            }
            else if (hJob_) {
                LOG_ERROR("ProcessManager", "Failed to assign process to Job Object. Error: " + std::to_string(GetLastError()));
            }

            CloseHandle(processInfo.hThread);
        }
        else {
            LOG_ERROR("ProcessManager", "CreateProcess failed for: " + commandLine +
                " with error: " + std::to_string(GetLastError()));
            return false;
        }
    }
    else {
        SHELLEXECUTEINFOA shExInfo = { 0 };
        shExInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
        shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
        shExInfo.lpVerb = "open";
        shExInfo.lpFile = exePathStr.c_str();
        shExInfo.lpParameters = args.c_str();
        shExInfo.lpDirectory = currDirStr.c_str();
        shExInfo.nShow = SW_SHOWNORMAL;

        if (ShellExecuteExA(&shExInfo)) {
            launchCommandSent = true;

            if (g_hasSteamGameRoot) {
                // Steam URL / app launch: Steam client is immediate child, not the game.
                LOG_INFO("ProcessManager",
                    "ShellExecuteEx started Steam (or helper); will detect real game in folder: "
                    + g_steamGameRoot);

                if (shExInfo.hProcess) {
                    CloseHandle(shExInfo.hProcess); // don�t hold Steam�s handle
                }
            }
            else if (shExInfo.hProcess) {
                // Non-Steam ShellExecute: treat as direct child process to monitor.
                hProcess_ = shExInfo.hProcess;
                processId_ = GetProcessId(hProcess_);

                if (hJob_ != NULL && AssignProcessToJobObject(hJob_, hProcess_)) {
                    jobAssigned_ = true;
                    LOG_INFO("ProcessManager", "Process (from ShellExecuteEx) assigned to Job Object.");
                }
                else if (hJob_ != NULL) {
                    LOG_WARNING("ProcessManager", "Failed to assign process from ShellExecuteEx to Job Object.");
                }
            }
            else {
                LOG_INFO("ProcessManager",
                    "ShellExecute did not return a process handle. Detection will occur in the wait phase.");
            }
        }
        else {
            LOG_ERROR("ProcessManager", "ShellExecuteEx failed for: " + exePathStr +
                " with error: " + std::to_string(GetLastError()));
            return false;
        }
    }

    return launchCommandSent;
}



WaitResult WindowsProcessManager::wait(
    double timeoutSeconds,
    const std::function<bool()>& userInputCheck,
    const FrameTickCallback& onFrameTick) {
    // If we don't yet have a running process handle, enter detection mode.
    bool isDetecting = !isRunning();
    if (isDetecting) {
        LOG_INFO("ProcessManager", "Entering detection phase (UI will remain active)...");
    }
    else {
        LOG_INFO("ProcessManager", "Process handle already acquired. Entering monitoring phase...");
    }

    auto monitoringStartTime = std::chrono::steady_clock::now();
    auto lastDetectionTime = monitoringStartTime;

    // Make sure we don't have stale foreground state from a previous launch
    pendingForeground_ = false;

    while (true) {
        // Pump UI / animations
        if (onFrameTick) {
            onFrameTick();
        }

        // Check for quitcombo / user input
        if (userInputCheck && userInputCheck()) {
            return WaitResult::UserInput;
        }

        auto now = std::chrono::steady_clock::now();

        // Foreground retry window:
        //  - After we attach (or re-attach) to a Steam/Epic PID,
        //    keep trying to bring its window forward until either:
        //      * forceForegroundForPid() returns true, OR
        //      * foregroundDeadline_ is reached.
        if (pendingForeground_ && processId_ != 0) {
            if (forceForegroundForPid(processId_) || now >= foregroundDeadline_) {
                // Either successfully focused something, or gave up after the deadline.
                pendingForeground_ = false;
            }
        }

        if (isDetecting) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDetectionTime).count() > 250) {
                const int focusGracePeriodSec = 5;
                auto      elapsedGrace = std::chrono::duration_cast<std::chrono::seconds>(now - monitoringStartTime).count();
                HWND      foregroundHwnd = GetForegroundWindow();

                // If after a short grace period focus has clearly bounced back to RetroFE,
                // treat it as a failed launch.
                if (elapsedGrace > focusGracePeriodSec && foregroundHwnd == hRetroFEWindow_) {
                    LOG_WARNING("ProcessManager", "Focus returned to RetroFE after grace period; assuming launch failed.");
                    return WaitResult::Error;
                }

                //
                // Steam-based detection: if we know the install root for this appid, scan
                // for any process whose EXE path lives under that folder. Attach to the
                // first one we find and transition into monitoring mode.
                //
                if (g_hasSteamGameRoot) {
                    DWORD gamePid = findProcessInFolder(g_steamGameRoot);
                    if (gamePid != 0) {
                        HANDLE hGame = OpenProcess(
                            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE,
                            gamePid);

                        if (hGame) {
                            // Attach to the real game (or splash) process
                            hProcess_ = hGame;
                            processId_ = gamePid;

                            // Optional: record friendly exe name for logging / special cases
                            char  pathBuf[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameA(hGame, 0, pathBuf, &size)) {
                                executableName_ = getExeNameFromPath(pathBuf);
                                LOG_INFO("ProcessManager",
                                    "Attached to Steam game process PID " +
                                    std::to_string(gamePid) +
                                    " EXE: " + executableName_);
                            }
                            else {
                                executableName_.clear();
                                LOG_INFO("ProcessManager",
                                    "Attached to Steam game process PID " +
                                    std::to_string(gamePid));
                            }

                            isDetecting = false;
                            LOG_INFO("ProcessManager", "Transitioning to monitoring phase (Steam game detected).");

                            // Schedule foreground attempts for the next few seconds
                            pendingForeground_ = true;
                            foregroundDeadline_ = now + std::chrono::seconds(5);
                        }
                        else {
                            LOG_WARNING("ProcessManager",
                                "Found Steam game PID " + std::to_string(gamePid) +
                                " but OpenProcess failed (err=" +
                                std::to_string(GetLastError()) + ")");
                        }
                    }
                }

                // Epic-based detection (parallel to Steam)
                if (isDetecting && g_hasEpicGameRoot) {
                    DWORD gamePid = findProcessInFolder(g_epicGameRoot);
                    if (gamePid != 0) {
                        HANDLE hGame = OpenProcess(
                            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE,
                            gamePid);

                        if (hGame) {
                            hProcess_ = hGame;
                            processId_ = gamePid;

                            char  pathBuf[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameA(hGame, 0, pathBuf, &size)) {
                                executableName_ = getExeNameFromPath(pathBuf);
                                LOG_INFO("ProcessManager",
                                    "Attached to Epic game process PID " +
                                    std::to_string(gamePid) +
                                    " EXE: " + executableName_);
                            }
                            else {
                                executableName_.clear();
                                LOG_INFO("ProcessManager",
                                    "Attached to Epic game process PID " +
                                    std::to_string(gamePid));
                            }

                            isDetecting = false;
                            LOG_INFO("ProcessManager", "Transitioning to monitoring phase (Epic game detected).");

                            // Schedule foreground attempts for the next few seconds
                            pendingForeground_ = true;
                            foregroundDeadline_ = now + std::chrono::seconds(5);
                        }
                        else {
                            LOG_WARNING("ProcessManager",
                                "Found Epic game PID " + std::to_string(gamePid) +
                                " but OpenProcess failed (err=" +
                                std::to_string(GetLastError()) + ")");
                        }
                    }
                }

                // In future, non-Steam detection heuristics (like fullscreen window scanning)
                // could also live here as an additional branch.

                lastDetectionTime = now;
            }
        }
        else {
            // Monitoring phase: we have a valid hProcess_ and processId_.
            DWORD waitRes = WaitForSingleObject(hProcess_, 0);
            if (waitRes == WAIT_OBJECT_0) {
                //
                // Steam/Epic re-attach logic:
                // If this was a Steam/Epic game and something *else* in that folder is now running
                // (e.g. splash -> main game), re-attach to the new process instead of
                // treating this as a final exit.
                //
                if (g_hasSteamGameRoot) {
                    DWORD newPid = findProcessInFolder(g_steamGameRoot);

                    // If we found a *different* process in the same game folder, re-attach.
                    if (newPid != 0 && newPid != processId_) {
                        HANDLE hNew = OpenProcess(
                            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE,
                            newPid);

                        if (hNew) {
                            // Close the old process handle; it has exited.
                            if (hProcess_) {
                                CloseHandle(hProcess_);
                            }

                            hProcess_ = hNew;
                            processId_ = newPid;

                            char  pathBuf[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameA(hNew, 0, pathBuf, &size)) {
                                executableName_ = getExeNameFromPath(pathBuf);
                                LOG_INFO("ProcessManager",
                                    "Previous Steam process exited; re-attached to new process PID " +
                                    std::to_string(newPid) +
                                    " EXE: " + executableName_);
                            }
                            else {
                                executableName_.clear();
                                LOG_INFO("ProcessManager",
                                    "Previous Steam process exited; re-attached to new process PID " +
                                    std::to_string(newPid));
                            }

                            // New main Steam game is now running � give it a foreground retry window
                            pendingForeground_ = true;
                            foregroundDeadline_ = now + std::chrono::seconds(5);

                            // Stay in monitoring phase; do NOT report ProcessExit yet.
                            // Loop continues and we keep monitoring hNew.
                            goto pump_messages;
                        }
                        else {
                            LOG_WARNING("ProcessManager",
                                "Detected new Steam process PID " + std::to_string(newPid) +
                                " but OpenProcess failed (err=" +
                                std::to_string(GetLastError()) + ")");
                        }
                    }
                }

                if (g_hasEpicGameRoot) {
                    DWORD newPid = findProcessInFolder(g_epicGameRoot);
                    if (newPid != 0 && newPid != processId_) {
                        HANDLE hNew = OpenProcess(
                            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE,
                            newPid);

                        if (hNew) {
                            if (hProcess_) {
                                CloseHandle(hProcess_);
                            }

                            hProcess_ = hNew;
                            processId_ = newPid;

                            char  pathBuf[MAX_PATH];
                            DWORD size = MAX_PATH;
                            if (QueryFullProcessImageNameA(hNew, 0, pathBuf, &size)) {
                                executableName_ = getExeNameFromPath(pathBuf);
                                LOG_INFO("ProcessManager",
                                    "Previous Epic process exited; re-attached to new process PID " +
                                    std::to_string(newPid) +
                                    " EXE: " + executableName_);
                            }
                            else {
                                executableName_.clear();
                                LOG_INFO("ProcessManager",
                                    "Previous Epic process exited; re-attached to new process PID " +
                                    std::to_string(newPid));
                            }

                            // New main Epic game is now running � give it a foreground retry window
                            pendingForeground_ = true;
                            foregroundDeadline_ = now + std::chrono::seconds(5);

                            // Continue monitoring new process
                            goto pump_messages;
                        }
                        else {
                            LOG_WARNING("ProcessManager",
                                "Detected new Epic process PID " + std::to_string(newPid) +
                                " but OpenProcess failed (err=" +
                                std::to_string(GetLastError()) + ")");
                        }
                    }
                }

                // No Steam/Epic game root, or no new PID to attach to => treat as real exit.
                return WaitResult::ProcessExit;
            }
        }

        // Timeout support (if a non-zero timeoutSeconds was provided)
        if (timeoutSeconds > 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                now - monitoringStartTime).count() >= timeoutSeconds)
            {
                return WaitResult::Timeout;
            }
        }

    pump_messages:
        // Keep message pump alive so RetroFE doesn't freeze while we wait.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
    }
}



void WindowsProcessManager::terminate() {
    // General grace vs MAME grace.
    // Kept separate because MAME may need a different value later.
    constexpr DWORD kGraceWaitMsGeneral = 15000;
    constexpr DWORD kGraceWaitMsMame = 15000;

    // If there is no running primary process AND no assigned job, fall back to
    // Steam/Epic folder-based cleanup. If a job still exists, do not bail here:
    // the primary process may have exited while helper processes remain in the job.
    if (!isRunning() && !(jobAssigned_ && hJob_)) {
        LOG_WARNING("ProcessManager",
            "Terminate called but no process was running; checking Steam/Epic folder fallback.");

        // Steam fallback: if we know the game root, kill processes in that folder.
        if (g_hasSteamGameRoot) {
            LOG_INFO("ProcessManager",
                "Attempting Steam-folder based termination for " + g_steamGameRoot);

            std::set<DWORD> processedIds;
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

            if (hSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32 pe32{};
                pe32.dwSize = sizeof(pe32);

                if (Process32First(hSnap, &pe32)) {
                    DWORD selfPid = GetCurrentProcessId();

                    do {
                        DWORD pid = pe32.th32ProcessID;
                        if (pid == 0 || pid == selfPid) {
                            continue;
                        }

                        if (isProcessInFolder(pid, g_steamGameRoot)) {
                            terminateProcessTree(pid, processedIds);
                        }
                    } while (Process32Next(hSnap, &pe32));
                }

                CloseHandle(hSnap);
            }
        }

        // Epic fallback: if we know the game root, kill processes in that folder.
        if (g_hasEpicGameRoot) {
            LOG_INFO("ProcessManager",
                "Attempting Epic-folder based termination for " + g_epicGameRoot);

            std::set<DWORD> processedIds;
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

            if (hSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32 pe32{};
                pe32.dwSize = sizeof(pe32);

                if (Process32First(hSnap, &pe32)) {
                    DWORD selfPid = GetCurrentProcessId();

                    do {
                        DWORD pid = pe32.th32ProcessID;
                        if (pid == 0 || pid == selfPid) {
                            continue;
                        }

                        if (isProcessInFolder(pid, g_epicGameRoot)) {
                            terminateProcessTree(pid, processedIds);
                        }
                    } while (Process32Next(hSnap, &pe32));
                }

                CloseHandle(hSnap);
            }
        }

        cleanupHandles();
        return;
    }

    const bool  isMame = isMameExeName(executableName_);
    const DWORD graceBudget = isMame ? kGraceWaitMsMame : kGraceWaitMsGeneral;

    bool                  createdMameTrigger = false;
    std::filesystem::path mameTriggerPath;

    auto removeTriggerIfNeeded = [&]() {
        if (!createdMameTrigger) {
            return;
        }

        std::error_code ec;
        std::filesystem::remove(mameTriggerPath, ec);
        };

    const auto t0 = std::chrono::steady_clock::now();

    auto remainingMs = [&]() -> DWORD {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (elapsed >= static_cast<long long>(graceBudget)) {
            return 0;
        }

        return static_cast<DWORD>(graceBudget - elapsed);
        };

    // --- MAME special-case: ask it to quit via trigger file first ---
    if (isMame && !workingDirectory_.empty()) {
        mameTriggerPath = std::filesystem::path(workingDirectory_) / "mame_exit.trigger";

        std::ofstream out(mameTriggerPath.string(), std::ios::out | std::ios::trunc);
        if (out.good()) {
            out.close();
            createdMameTrigger = true;

            LOG_INFO("ProcessManager",
                "MAME detected - wrote exit trigger: " + mameTriggerPath.string());

            DWORD rem = remainingMs();

            bool mameExited = false;
            if (rem > 0) {
                if (jobAssigned_ && hJob_) {
                    // Passive wait. The trigger file is the shutdown request.
                    // Do not use primary PID exit as success while a job exists.
                    mameExited = waitForJobEmptyBounded(hJob_, rem);
                }
                else {
                    mameExited = waitForPidExitBounded(processId_, rem);
                }
            }

            if (mameExited) {
                LOG_INFO("ProcessManager", "MAME exited cleanly after trigger.");
                removeTriggerIfNeeded();
                cleanupHandles();
                return;
            }

            std::error_code ec;
            std::filesystem::remove(mameTriggerPath, ec);

            if (ec) {
                LOG_WARNING("ProcessManager",
                    "Failed to remove MAME exit trigger (" +
                    mameTriggerPath.string() + "): " + ec.message());
            }
            else {
                LOG_INFO("ProcessManager",
                    "MAME did not exit in time; removed exit trigger and escalating.");
            }

            createdMameTrigger = false;
        }
        else {
            LOG_WARNING("ProcessManager",
                "MAME detected but could not write exit trigger: " +
                mameTriggerPath.string());
        }
    }

    // --- If we have a job, prefer job-based graceful shutdown ---
    if (jobAssigned_ && hJob_) {
        LOG_INFO("ProcessManager",
            "Attempting graceful shutdown for job (primary PID " +
            std::to_string(processId_) + ")...");

        DWORD rem = remainingMs();

        if (rem > 0 && requestGracefulShutdownForJob(rem)) {
            removeTriggerIfNeeded();
            cleanupHandles();
            return;
        }

        LOG_WARNING("ProcessManager",
            "Graceful job shutdown failed; escalating to TerminateJobObject.");

        removeTriggerIfNeeded();
        TerminateJobObject(hJob_, 1);
        cleanupHandles();
        return;
    }

    // --- No job: graceful shutdown for the single PID ---
    LOG_INFO("ProcessManager",
        "Attempting graceful shutdown for PID " + std::to_string(processId_) + "...");

    {
        DWORD rem = remainingMs();

        if (rem > 0 && requestGracefulShutdownForPid(processId_, rem)) {
            LOG_INFO("ProcessManager", "Graceful shutdown succeeded.");
            removeTriggerIfNeeded();
            cleanupHandles();
            return;
        }
    }

    // This check is valid only in the non-job path.
    if (!isRunning()) {
        LOG_INFO("ProcessManager",
            "Primary process already exited; treating as graceful success.");

        removeTriggerIfNeeded();
        cleanupHandles();
        return;
    }

    LOG_WARNING("ProcessManager",
        "Graceful shutdown failed; terminating process tree.");

    removeTriggerIfNeeded();

    std::set<DWORD> processedIds;
    terminateProcessTree(processId_, processedIds);

    cleanupHandles();
}

bool WindowsProcessManager::tryGetExitCode(int& outExitCode) const {
    if (!hProcess_) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(hProcess_, &code)) return false;
    if (code == STILL_ACTIVE) return false;
    outExitCode = static_cast<int>(code);
    return true;
}

bool WindowsProcessManager::isRunning() const {
    if (hProcess_ == nullptr) return false;
    DWORD exitCode = 0;
    return (GetExitCodeProcess(hProcess_, &exitCode) && exitCode == STILL_ACTIVE);
}

void WindowsProcessManager::terminateProcessTree(DWORD processId, std::set<DWORD>& processedIds) {
    if (processedIds.count(processId)) return;
    processedIds.insert(processId);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32{};
        pe32.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnap, &pe32)) {
            do {
                if (pe32.th32ParentProcessID == processId) {
                    terminateProcessTree(pe32.th32ProcessID, processedIds);
                }
            } while (Process32Next(hSnap, &pe32));
        }
        CloseHandle(hSnap);
    }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
    if (hProc) {
        LOG_DEBUG("ProcessManager", "Terminating PID: " + std::to_string(processId));
        TerminateProcess(hProc, 1);
        CloseHandle(hProc);
    }
}

#endif
