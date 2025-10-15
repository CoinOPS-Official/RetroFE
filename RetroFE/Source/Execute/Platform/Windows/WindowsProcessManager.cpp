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

#include <filesystem>
#include <Psapi.h> // For GetModuleFileNameExA
#include <tlhelp32.h> // For CreateToolhelp32Snapshot
#include <SDL2/SDL_syswm.h>
#include "../../../SDL.h" // For SDL::getActiveWindow

#include "../../../Utility/Log.h"
#include "../../../Utility/Utils.h" // For Utils::toLower
#include "../../../Database/Configuration.h" // For Configuration::absolutePath

namespace {
    // Utility to get the base name of an executable from a full path
    std::string getExeNameFromPath(const std::string& path) {
        auto pos = path.find_last_of("/\\");
        if (pos != std::string::npos) {
            return path.substr(pos + 1);
        }
        return path;
    }
}

WindowsProcessManager::WindowsProcessManager() {
    LOG_INFO("ProcessManager", "WindowsProcessManager created.");

    // Get and store RetroFE's main window handle for focus checks.
    SDL_SysWMinfo winfo; // Renamed to avoid shadowing version macro
    SDL_VERSION(&winfo.version);

    // --- THIS IS THE CORRECTED AND ROBUST CHECK ---
    SDL_Window* mainWindow = SDL::getWindow(0);
    if (mainWindow != nullptr && SDL_GetWindowWMInfo(mainWindow, &winfo) == SDL_TRUE) {
        // This code is only executed if SDL is loaded, the window exists,
        // and the WM info was successfully retrieved.
        hRetroFEWindow_ = winfo.info.win.window;
    }
    else {
        // This code is now correctly executed if SDL is unloaded OR
        // if the WM info call fails for any other reason.
        hRetroFEWindow_ = nullptr;
    }
}

WindowsProcessManager::~WindowsProcessManager() {
    cleanupHandles();
}

// Collect all top - level windows that belong to a PID
static void collectWindowsForPid(DWORD pid, std::vector<HWND>&out) {
    struct Ctx { DWORD pid; std::vector<HWND>* out; };
    auto thunk = [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* ctx = reinterpret_cast<Ctx*>(lParam);
        DWORD winPid = 0; GetWindowThreadProcessId(hwnd, &winPid);
        if (winPid == ctx->pid && IsWindow(hwnd) && IsWindowVisible(hwnd)) {
            ctx->out->push_back(hwnd);
        }
        return TRUE;
        };
    Ctx ctx{ pid, &out };
    EnumWindows(thunk, reinterpret_cast<LPARAM>(&ctx));
}

// Politely ask each window to close (never blocks indefinitely)
static void sendCloseToWindows(const std::vector<HWND>& windows) {
    for (HWND h : windows) {
        // Try the standard close command first
        SendMessageTimeout(h, WM_SYSCOMMAND, SC_CLOSE, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, nullptr);
        // Follow with WM_CLOSE in case SC_CLOSE is ignored
        SendMessageTimeout(h, WM_CLOSE, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, nullptr);
    }
}

// Best-effort, bounded wait for a single PID to exit
static bool waitForPidExitBounded(DWORD pid, DWORD waitMsTotal) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;

    const DWORD slice = 100;
    DWORD waited = 0;
    while (waited < waitMsTotal) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            return true;
        }
        // keep UI responsive (matches your main wait loop style)
        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
        waited += slice;
    }
    CloseHandle(h);
    return false;
}

// Graceful ask for a single PID
bool WindowsProcessManager::requestGracefulShutdownForPid(DWORD pid, DWORD waitMsTotal) {
    std::vector<HWND> hwnds;
    collectWindowsForPid(pid, hwnds);
    if (!hwnds.empty()) {
        LOG_INFO("ProcessManager", "Sending close to " + std::to_string(hwnds.size()) + " window(s) for PID " + std::to_string(pid));
        sendCloseToWindows(hwnds);
        return waitForPidExitBounded(pid, waitMsTotal);
    }
    // No windows — nothing to ask nicely here.
    return false;
}

// Graceful ask for all processes in our Job
bool WindowsProcessManager::requestGracefulShutdownForJob(DWORD waitMsTotal) {
    if (!hJob_) return false;

    // Query the list size
    JOBOBJECT_BASIC_PROCESS_ID_LIST header{ 0 };
    DWORD bytes = 0;
    (void)QueryInformationJobObject(hJob_, JobObjectBasicProcessIdList, &header, sizeof(header), &bytes);
    if (bytes == 0) return false;

    std::vector<BYTE> buf(bytes);
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
    if (!QueryInformationJobObject(hJob_, JobObjectBasicProcessIdList, list, bytes, &bytes)) {
        return false;
    }

    // Ask all job members to close
    for (ULONG i = 0; i < list->NumberOfProcessIdsInList; ++i) {
        DWORD pid = static_cast<DWORD>(list->ProcessIdList[i]);
        std::vector<HWND> hwnds;
        collectWindowsForPid(pid, hwnds);
        if (!hwnds.empty()) {
            LOG_INFO("ProcessManager", "Requesting close for job member PID " + std::to_string(pid));
            sendCloseToWindows(hwnds);
        }
    }

    // Bounded wait: succeed if all exited before timeout
    const DWORD slice = 100;
    DWORD waited = 0;
    std::vector<HANDLE> handles;
    handles.reserve(list->NumberOfProcessIdsInList);
    for (ULONG i = 0; i < list->NumberOfProcessIdsInList; ++i) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(list->ProcessIdList[i]));
        if (h) handles.push_back(h);
    }

    while (waited < waitMsTotal) {
        bool allExited = true;
        for (HANDLE h : handles) {
            if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) { allExited = false; break; }
        }
        if (allExited) {
            for (HANDLE h : handles) CloseHandle(h);
            return true;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
        waited += slice;
    }

    for (HANDLE h : handles) CloseHandle(h);
    return false;
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
}

// --- Public Interface Implementation ---

bool WindowsProcessManager::simpleLaunch(const std::string& executable,
    const std::string& args,
    const std::string& currentDirectory) {
    std::string ext = Utils::toLower(std::filesystem::path(executable).extension().string());
    bool isBatch = (ext == ".bat" || ext == ".cmd");

    std::string commandLine;
    std::string workDir = currentDirectory;

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // hide if anything is created

    if (isBatch) {
        // Use COMSPEC /C "bat args" — completely windowless
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

bool WindowsProcessManager::launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) {
    // Ensure we start fresh
    cleanupHandles();

    // The user's provided path should already be absolute, but this is a good safety check.
    std::filesystem::path exePath(executable);
    if (!exePath.is_absolute()) {
        exePath = std::filesystem::absolute(exePath);
    }
    std::filesystem::path currDir(currentDirectory);
    if (!currDir.is_absolute()) {
        currDir = std::filesystem::absolute(currDir);
    }

    std::string exePathStr = exePath.string();
    std::string currDirStr = currDir.string();
    executableName_ = getExeNameFromPath(exePathStr);

    LOG_INFO("ProcessManager", "Attempting to launch: " + exePathStr);
    if (!args.empty()) {
        LOG_INFO("ProcessManager", "           Arguments: " + args);
    }
    LOG_INFO("ProcessManager", "     Working directory: " + currDirStr);

    // --- Create and configure the Job Object for potential use ---
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

    // --- Determine Launch Type and Execute ---
    std::string extension = Utils::toLower(exePath.extension().string());
    bool isExeOrBat = (extension == ".exe" || extension == ".bat");
    bool launchCommandSent = false;

    if (isExeOrBat) {
        STARTUPINFOA startupInfo{};
        PROCESS_INFORMATION processInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.wShowWindow = SW_SHOWDEFAULT;

        std::string commandLine = "\"" + exePathStr + "\"";
        if (!args.empty()) {
            commandLine += " " + args;
        }

        if (CreateProcessA(nullptr, &commandLine[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, currDirStr.c_str(), &startupInfo, &processInfo)) {
            launchCommandSent = true;
            hProcess_ = processInfo.hProcess;
            processId_ = processInfo.dwProcessId;

            if (hJob_ != NULL && AssignProcessToJobObject(hJob_, hProcess_)) {
                jobAssigned_ = true;
                LOG_INFO("ProcessManager", "Process assigned to Job Object.");
            }
            else if (hJob_ != NULL) {
                LOG_ERROR("ProcessManager", "Failed to assign process to Job Object. Error: " + std::to_string(GetLastError()));
            }

            if (ResumeThread(processInfo.hThread) == (DWORD)-1) {
                LOG_ERROR("ProcessManager", "Failed to resume process thread. Error: " + std::to_string(GetLastError()));
                terminate(); // Attempt to clean up the failed launch
                return false;
            }
            CloseHandle(processInfo.hThread);
        }
        else {
            LOG_ERROR("ProcessManager", "CreateProcess failed for: " + commandLine + " with error: " + std::to_string(GetLastError()));
            return false;
        }
    }
    else { // Use ShellExecute for other file types
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
            if (shExInfo.hProcess) {
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
                // This is the expected outcome for complex launches like Steam.
                LOG_INFO("ProcessManager", "ShellExecute did not return a process handle. Detection will occur in the wait phase.");
            }
        }
        else {
            LOG_ERROR("ProcessManager", "ShellExecuteEx failed for: " + exePathStr + " with error: " + std::to_string(GetLastError()));
            return false;
        }
    }

    return launchCommandSent;
}

WaitResult WindowsProcessManager::wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) {
    bool isDetecting = !isRunning(); // Start in detection phase if we don't have a handle.
    if (isDetecting) {
        LOG_INFO("ProcessManager", "Entering detection phase (UI will remain active)...");
    }
    else {
        LOG_INFO("ProcessManager", "Process handle already acquired. Entering monitoring phase...");
    }

    auto monitoringStartTime = std::chrono::steady_clock::now();
    auto lastDetectionTime = monitoringStartTime; // Timer for throttling detection checks.
    HWND lastLoggedHwnd = nullptr; // For anti-spam in detection logging.

    // This is the main loop for both detection and monitoring.
    while (true) {
        // --- ALWAYS RENDER AND CHECK INPUT ---
        // This runs at the full frequency of the loop (~33ms, or 30fps).
        if (onFrameTick) {
            onFrameTick();
        }
        if (userInputCheck && userInputCheck()) {
            return WaitResult::UserInput;
        }

        // --- PHASE 1: DETECTION LOGIC ---
        if (isDetecting) {
            auto now = std::chrono::steady_clock::now();

            // Throttle the expensive checks to run every ~250ms.
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDetectionTime).count() > 250) {
                const int focusGracePeriodSec = 5;
                auto elapsedGrace = std::chrono::duration_cast<std::chrono::seconds>(now - monitoringStartTime).count();
                HWND foregroundHwnd = GetForegroundWindow();

                if (elapsedGrace > focusGracePeriodSec && foregroundHwnd == hRetroFEWindow_) {
                    LOG_WARNING("ProcessManager", "Focus returned to RetroFE after grace period; assuming launch failed.");
                    return WaitResult::Error; // Detection failed.
                }

                if (foregroundHwnd) {
                    DWORD pid;
                    GetWindowThreadProcessId(foregroundHwnd, &pid);

                    // Apply all filters before the final fullscreen check
                    if (pid != GetCurrentProcessId() && IsWindowVisible(foregroundHwnd)) {
                        if (isSteamHelperWindow(foregroundHwnd)) {
                            if (foregroundHwnd != lastLoggedHwnd) {
                                LOG_DEBUG("ProcessManager", "Ignoring known launcher window (Steam).");
                                lastLoggedHwnd = foregroundHwnd;
                            }
                        }
                        else {
                            // This window is a candidate. Check if it's fullscreen.
                            if (isWindowFullscreen(foregroundHwnd)) {
                                HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
                                if (hProc) {
                                    // SUCCESS! We found the handle.
                                    char windowTitle[256] = { 0 };
                                    GetWindowTextA(foregroundHwnd, windowTitle, sizeof(windowTitle));
                                    std::string exeName = getExeNameFromHwnd(foregroundHwnd);
                                    LOG_INFO("ProcessManager", "Detection successful. Found fullscreen game process (PID: " + std::to_string(pid) + ", Title: \"" + std::string(windowTitle) + "\", EXE: " + exeName + ").");

                                    LOG_INFO("ProcessManager", "Forcing detected window to the foreground.");
                                    SetForegroundWindow(foregroundHwnd);

                                    hProcess_ = hProc;
                                    processId_ = pid;
                                    executableName_ = exeName;
                                    jobAssigned_ = false;

                                    LOG_INFO("ProcessManager", "Transitioning to monitoring phase.");
                                    isDetecting = false; // <-- Transition to Phase 2!
                                }
                            }
                            else {
                                // It's a candidate, but not fullscreen. Log details for debugging.
                                if (foregroundHwnd != lastLoggedHwnd) {
                                    logFullscreenCheckDetails(foregroundHwnd);
                                    lastLoggedHwnd = foregroundHwnd;
                                }
                            }
                        }
                    }
                }
                lastDetectionTime = now; // Reset the throttle timer.
            }
        }
        // --- PHASE 2: MONITORING LOGIC ---
        else {
            if (WaitForSingleObject(hProcess_, 0) == WAIT_OBJECT_0) {
                return WaitResult::ProcessExit;
            }
        }

        // --- GLOBAL TIMEOUT CHECK (for attract mode) ---
        if (timeoutSeconds > 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - monitoringStartTime).count() >= timeoutSeconds) {
                return WaitResult::Timeout;
            }
        }

        // --- MESSAGE PUMP and FINE-GRAINED WAIT ---
        // Use a fine-grained wait for responsiveness, allowing the loop to run at ~30fps.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
    }
}

void WindowsProcessManager::terminate() {
    constexpr DWORD kGraceWaitMs = 3000; // tune as desired (1–5s typical)

    if (jobAssigned_ && hJob_) {
        LOG_INFO("ProcessManager", "Attempting graceful shutdown for job...");
        if (requestGracefulShutdownForJob(kGraceWaitMs)) {
            LOG_INFO("ProcessManager", "Graceful job shutdown succeeded.");
            cleanupHandles();
            return;
        }
        LOG_WARNING("ProcessManager", "Graceful job shutdown failed; escalating to TerminateJobObject.");
        TerminateJobObject(hJob_, 1);
        cleanupHandles();
        return;
    }

    if (isRunning()) {
        LOG_INFO("ProcessManager", "Attempting graceful shutdown for PID " + std::to_string(processId_) + "...");
        if (requestGracefulShutdownForPid(processId_, kGraceWaitMs)) {
            LOG_INFO("ProcessManager", "Graceful shutdown succeeded.");
            cleanupHandles();
            return;
        }
        LOG_WARNING("ProcessManager", "Graceful shutdown failed; terminating process tree.");
        std::set<DWORD> processedIds;
        terminateProcessTree(processId_, processedIds);
        cleanupHandles();
        return;
    }

    LOG_WARNING("ProcessManager", "Terminate called but no process was running.");
    cleanupHandles();
}

bool WindowsProcessManager::isRunning() const {
    if (hProcess_ == nullptr) {
        return false;
    }
    // Check if the process is still active.
    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess_, &exitCode) && exitCode == STILL_ACTIVE) {
        return true;
    }
    return false;
}

// --- Private Helper Implementations ---

std::string WindowsProcessManager::getExeNameFromHwnd(HWND hwnd) {
    if (!hwnd) return "";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return "";

    char exePath[MAX_PATH] = { 0 };
    std::string exeName;
    if (GetModuleFileNameExA(hProc, NULL, exePath, MAX_PATH)) {
        // Assuming getExeNameFromPath is still in the anonymous namespace, which is fine.
        exeName = getExeNameFromPath(exePath);
    }
    CloseHandle(hProc);
    return exeName;
}

void WindowsProcessManager::logFullscreenCheckDetails(HWND hwnd) {
    if (!hwnd) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::string exeName = getExeNameFromHwnd(hwnd);

    RECT appBounds;
    if (!GetWindowRect(hwnd, &appBounds)) {
        LOG_DEBUG("ProcessManager", "FullscreenCheck: GetWindowRect failed for " + exeName);
        return;
    }

    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor) {
        LOG_DEBUG("ProcessManager", "FullscreenCheck: MonitorFromWindow failed for " + exeName);
        return;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
        LOG_DEBUG("ProcessManager", "FullscreenCheck: GetMonitorInfo failed for " + exeName);
        return;
    }

    char windowTitle[256] = { 0 };
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));

    // Format the rectangles into a clear string for comparison
    std::string windowRectStr = "L:" + std::to_string(appBounds.left) +
        " T:" + std::to_string(appBounds.top) +
        " R:" + std::to_string(appBounds.right) +
        " B:" + std::to_string(appBounds.bottom);

    std::string monitorRectStr = "L:" + std::to_string(monitorInfo.rcMonitor.left) +
        " T:" + std::to_string(monitorInfo.rcMonitor.top) +
        " R:" + std::to_string(monitorInfo.rcMonitor.right) +
        " B:" + std::to_string(monitorInfo.rcMonitor.bottom);

    LOG_DEBUG("ProcessManager", "Fullscreen Check Failed for \"" + std::string(windowTitle) +
        "\" (PID: " + std::to_string(pid) + ", EXE: " + exeName +
        "). Window: {" + windowRectStr + "} | Monitor: {" + monitorRectStr + "}");
}

void WindowsProcessManager::terminateProcessTree(DWORD processId, std::set<DWORD>& processedIds) {
    if (processedIds.count(processId)) return;
    processedIds.insert(processId);

    // First, recursively terminate all children.
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

    // Now, terminate the parent.
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
    if (hProc) {
        LOG_DEBUG("ProcessManager", "Terminating PID: " + std::to_string(processId));
        TerminateProcess(hProc, 1);
        CloseHandle(hProc);
    }
}

// --- Static Helpers ---

bool WindowsProcessManager::isWindowFullscreen(HWND hwnd) {
    if (!hwnd) return false;

    RECT appBounds;
    if (!GetWindowRect(hwnd, &appBounds)) return false;

    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor) return false;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hMonitor, &monitorInfo)) return false;

    const int tolerance = 4; // A small pixel tolerance for minor differences.

    // Case 1: True fullscreen or near-fullscreen borderless.
    // Check if the window's dimensions are very close to the monitor's.
    long windowWidth = appBounds.right - appBounds.left;
    long windowHeight = appBounds.bottom - appBounds.top;
    long monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    long monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

    if (abs(windowWidth - monitorWidth) <= tolerance &&
        abs(windowHeight - monitorHeight) <= tolerance) {
        // Also check if it's positioned at the top-left corner.
        if (abs(appBounds.left - monitorInfo.rcMonitor.left) <= tolerance &&
            abs(appBounds.top - monitorInfo.rcMonitor.top) <= tolerance) {
            return true; // This handles standard fullscreen and simple borderless.
        }
    }

    // Case 2: Overscan/Negative Margin Fullscreen (like DRAGON BALL FighterZ).
    // Check if the monitor's rectangle is contained within the window's rectangle.
    if (appBounds.left <= monitorInfo.rcMonitor.left &&
        appBounds.top <= monitorInfo.rcMonitor.top &&
        appBounds.right >= monitorInfo.rcMonitor.right &&
        appBounds.bottom >= monitorInfo.rcMonitor.bottom) {
        return true; // The window completely envelops the screen.
    }

    return false;
}

bool WindowsProcessManager::isSteamHelperWindow(HWND hwnd) {
    if (!hwnd) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return false;

    char exePath[MAX_PATH] = { 0 };
    bool result = false;
    if (GetModuleFileNameExA(hProc, NULL, exePath, MAX_PATH)) {
        // Get the base executable name once.
        std::string exeName = getExeNameFromPath(exePath);

        // Check if it's either of the known Steam executables.
        if (_stricmp(exeName.c_str(), "steamwebhelper.exe") == 0 ||
            _stricmp(exeName.c_str(), "steam.exe") == 0) {
            result = true;
        }
    }

    CloseHandle(hProc);
    return result;
}

#endif