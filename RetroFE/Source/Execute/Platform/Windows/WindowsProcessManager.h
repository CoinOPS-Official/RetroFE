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
#pragma once

#include <Windows.h> // For HANDLE, DWORD, HWND, etc.
#include <string>
#include <set>

#include "../IProcessManager.h" // Include the interface we are implementing

/**
 * @brief A Windows-specific implementation of the IProcessManager interface.
 *
 * This class handles the launching, monitoring, and termination of processes
 * using the Windows API, including Job Objects for robust child-process cleanup
 * and a fallback mechanism to find fullscreen game windows.
 */
class WindowsProcessManager : public IProcessManager {
public:
    /**
     * @brief Constructs the WindowsProcessManager.
     */
    WindowsProcessManager();

    /**
     * @brief Destructor that ensures all owned handles are closed.
     */
    ~WindowsProcessManager() override;

    // --- Public Interface Implementation ---

    bool simpleLaunch(const std::string& executable, const std::string& args, const std::string& currentDirectory) override;
    bool launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) override;
    WaitResult wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) override;
    void terminate() override;
    bool tryGetExitCode(int& outExitCode) const override;
    // This class manages unique system resources and should not be copied.
    WindowsProcessManager(const WindowsProcessManager&) = delete;
    WindowsProcessManager& operator=(const WindowsProcessManager&) = delete;

private:
    // --- Private Helper Methods ---

    static std::string getExeNameFromHwnd(HWND hwnd);

    static void logFullscreenCheckDetails(HWND hwnd);

    /**
     * @brief Recursively terminates a process and all of its child processes.
     * @param processId The ID of the parent process to terminate.
     * @param processedIds A set to track already-terminated PIDs to prevent infinite loops.
     */
    void terminateProcessTree(DWORD processId, std::set<DWORD>& processedIds);
    bool requestGracefulShutdownForPid(DWORD pid, DWORD waitMsTotal);
    bool requestGracefulShutdownForJob(DWORD waitMsTotal);

    /**
     * @brief A static helper to check if a window is truly fullscreen.
     * @param hwnd The handle to the window to check.
     * @return True if the window dimensions match the monitor dimensions.
     */
    static bool isWindowFullscreen(HWND hwnd);

    /**
     * @brief A static helper to identify and ignore Steam's web helper popup windows.
     * @param hwnd The handle to the window to check.
     * @return True if the window belongs to "steamwebhelper.exe".
     */
    static bool isSteamHelperWindow(HWND hwnd);

    /**
     * @brief Releases all owned system handles (process and job).
     */
    void cleanupHandles();

    bool isRunning() const;

    // --- Member Variables ---
	HWND hRetroFEWindow_ = nullptr; // Handle to the RetroFE main window, used for focus checks.
    HANDLE hProcess_ = nullptr;     // Handle to the main launched process.
    HANDLE hJob_ = nullptr;         // Handle to the Job Object for child process management.
    bool jobAssigned_ = false;      // True if the process was successfully added to the job.
    std::string executableName_;    // The base name of the launched executable (e.g., "mame.exe").
    DWORD processId_ = 0;           // The process ID of the launched process.
};
#endif