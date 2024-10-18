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

#include "Launcher.h"
#include "../RetroFE.h"
#include "../Collection/Item.h"
#include "../Utility/Log.h"
#include "../Database/Configuration.h"
#include "../Utility/Utils.h"
#include "../RetroFE.h"
#include "../SDL.h"
#include "../Database/GlobalOpts.h"
#include "../Collection/CollectionInfoBuilder.h"
#include <cstdlib>
#include <locale>
#include <sstream>
#include <fstream>
#include "../Graphics/Page.h"
#include <thread>
#include <atomic>
#include <filesystem>
#ifdef WIN32
#include <Windows.h>
#pragma comment(lib, "Xinput.lib")
#include <Xinput.h>
#include <cstring>
#include "PacDrive.h"
#include "StdAfx.h"
#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;

Launcher::Launcher(Configuration &c, RetroFE &retroFe)
    : config_(c),
    retroFeInstance_(retroFe)
{
}

std::string replaceVariables(std::string str,
    const std::string& itemFilePath,
    const std::string& itemName,
    const std::string& itemFilename,
    const std::string& itemDirectory,
    const std::string& itemCollectionName)
{
    str = Utils::replace(str, "%ITEM_FILEPATH%", itemFilePath);
    str = Utils::replace(str, "%ITEM_NAME%", itemName);
    str = Utils::replace(str, "%ITEM_FILENAME%", itemFilename);
    str = Utils::replace(str, "%ITEM_DIRECTORY%", itemDirectory);
    str = Utils::replace(str, "%ITEM_COLLECTION_NAME%", itemCollectionName);
    str = Utils::replace(str, "%RETROFE_PATH%", Configuration::absolutePath);
    str = Utils::replace(str, "%COLLECTION_PATH%", Utils::combinePath(Configuration::absolutePath, "collections", itemCollectionName));
#ifdef WIN32
    str = Utils::replace(str, "%RETROFE_EXEC_PATH%", Utils::combinePath(Configuration::absolutePath, "retrofe", "RetroFE.exe"));
    const char* comspec = std::getenv("COMSPEC");
    if (comspec) {
        str = Utils::replace(str, "%CMD%", std::string(comspec));
    }
#else
    str = Utils::replace(str, "%RETROFE_EXEC_PATH%", Utils::combinePath(Configuration::absolutePath, "RetroFE"));
#endif

    return str;
}

#ifdef WIN32
// Utility function to terminate a process and all its child processes
void TerminateProcessAndChildren(DWORD processId) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        LOG_WARNING("Launcher", "Failed to create snapshot for process termination.");
        return;
    }

    PROCESSENTRY32 pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // Get the first process
    if (Process32First(hSnap, &pe32)) {
        do {
            // Check if the process is a child of the given processId
            if (pe32.th32ParentProcessID == processId) {
                // Recursively terminate child processes
                TerminateProcessAndChildren(pe32.th32ProcessID);
            }
        } while (Process32Next(hSnap, &pe32));
    }

    CloseHandle(hSnap);

    // Open the main process and terminate it
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
    if (hProcess != nullptr) {
        LOG_INFO("Launcher", "Terminating process ID: " + std::to_string(processId));
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
    } else {
        LOG_WARNING("Launcher", "Failed to open process ID: " + std::to_string(processId));
    }
}
#endif

bool Launcher::run(std::string collection, Item* collectionItem, Page* currentPage, bool isAttractMode)
{
    // Initialize with the default launcher for the collection
    std::string launcherName = collectionItem->collectionInfo->launcher;

    // Check for per-item launcher override
    std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collection, "launchers", collectionItem->name + ".conf");
    if (std::ifstream launcherStream(launcherFile); launcherStream.good()) {
        std::string line;
        if (std::getline(launcherStream, line)) {
            // Construct localLauncher key
            std::string localLauncherKey = "localLaunchers." + collection + "." + line;
            if (config_.propertyPrefixExists(localLauncherKey)) {
                // Use localLauncher if exists
                launcherName = collection + "." + line;
            } else {
                // Use specified launcher from conf file
                launcherName = line;
            }
        }
    }

    // If no per-item launcher override, check for a collection-specific launcher
    if (launcherName == collectionItem->collectionInfo->launcher) {
        std::string collectionSpecificLauncherKey = "collectionLaunchers." + collection;
        if (config_.propertyPrefixExists(collectionSpecificLauncherKey)) {
            launcherName = collectionItem->collectionInfo->name; // Use the collection-specific launcher
        }
    }

    std::string executablePath;
    std::string selectedItemsDirectory;
    std::string selectedItemsPath;
    std::string extensionstr;
    std::string matchedExtension;
    std::string args;

    if (!launcherExecutable(executablePath, launcherName)) {
        LOG_ERROR("Launcher", "Failed to find launcher executable (launcher: " + launcherName + " executable: " + executablePath + " collection: " + collectionItem->collectionInfo->name + " item: " + collectionItem->name + ")");
        return false;
    }
    if (!extensions(extensionstr, collection)) {
        LOG_ERROR("Launcher", "No file extensions configured for collection \"" + collection + "\"");
        return false;
    }
    if (!collectionDirectory(selectedItemsDirectory, collection)) {
        LOG_ERROR("Launcher", "Could not find files in directory \"" + selectedItemsDirectory + "\" for collection \"" + collection + "\"");
        return false;
    }
    launcherArgs(args, launcherName); // Ok if no args

    // Overwrite selectedItemsDirectory if already set in the file
    if (!collectionItem->filepath.empty()) {
        selectedItemsDirectory = collectionItem->filepath;
    }

    LOG_DEBUG("LauncherDebug", "selectedItemsPath pre-find file: " + selectedItemsPath);
    LOG_DEBUG("LauncherDebug", "selectedItemsDirectory pre - find file : " + selectedItemsDirectory);
    LOG_DEBUG("LauncherDebug", "matchedExtension pre - find file : " + matchedExtension);
    LOG_DEBUG("LauncherDebug", "extensionstr pre - find file : " + extensionstr);
    LOG_DEBUG("LauncherDebug", "collectionItem->name pre - find file: " + collectionItem->name);
    LOG_DEBUG("LauncherDebug", "collectionItem->file pre - find file: " + collectionItem->file);

    // It is ok to continue if the file could not be found. We could be launching a merged romset
    if (collectionItem->file.empty()) {
        findFile(selectedItemsPath, matchedExtension, selectedItemsDirectory, collectionItem->name, extensionstr);
    } else {
        findFile(selectedItemsPath, matchedExtension, selectedItemsDirectory, collectionItem->file, extensionstr);
    }

    LOG_DEBUG("LauncherDebug", "args: " + args);
    LOG_DEBUG("LauncherDebug", "selectedItemsPath: " + selectedItemsPath);

    args = replaceVariables(args,
        selectedItemsPath,
        collectionItem->name,
        Utils::getFileName(selectedItemsPath),
        selectedItemsDirectory,
        collection);

    LOG_DEBUG("LauncherDebug", "executablePath: " + executablePath);

    executablePath = replaceVariables(executablePath,
        selectedItemsPath,
        collectionItem->name,
        Utils::getFileName(selectedItemsPath),
        selectedItemsDirectory,
        collection);

    std::string currentDirectoryKey = "launchers." + launcherName + ".currentDirectory";
    std::string currentDirectory = Utils::getDirectory(executablePath);

    config_.getProperty(currentDirectoryKey, currentDirectory);

    currentDirectory = replaceVariables(currentDirectory,
        selectedItemsPath,
        collectionItem->name,
        Utils::getFileName(selectedItemsPath),
        selectedItemsDirectory,
        collection);

    if (!execute(executablePath, args, currentDirectory, true, currentPage, isAttractMode, collectionItem)) {
        LOG_ERROR("Launcher", "Failed to launch.");
        return false;
    }

    bool reboot = false;
    config_.getProperty("launchers." + launcherName + ".reboot", reboot);

    return reboot;
}

void Launcher::startScript()
{
#ifdef WIN32
    std::string exe = Utils::combinePath(Configuration::absolutePath, "start.bat");
#else
    std::string exe = Utils::combinePath(Configuration::absolutePath, "start.sh");
#endif
    if(fs::exists(exe)) {
        simpleExecute(exe, "", Configuration::absolutePath, false);
    }
}

void Launcher::exitScript()
{
#ifdef WIN32
    std::string exe = Utils::combinePath(Configuration::absolutePath, "exit.bat");
#else
    std::string exe = Utils::combinePath(Configuration::absolutePath, "exit.sh");
#endif
    if(fs::exists(exe)) {
        simpleExecute(exe, "", Configuration::absolutePath, false);
    }
}

void Launcher::LEDBlinky( int command, std::string collection, Item *collectionItem )
{
	std::string LEDBlinkyDirectory = "";
	config_.getProperty( OPTION_LEDBLINKYDIRECTORY, LEDBlinkyDirectory );
	if (LEDBlinkyDirectory == "") {
        return;
    }
    std::string exe  = Utils::combinePath(LEDBlinkyDirectory, "LEDBlinky.exe");
    // Check if the LEDBlinky.exe file exists
    if (!std::filesystem::exists(exe)) {
        return; // Exit early if the file does not exist
    }
    std::string args = std::to_string( command );
	bool wait = false;
	if ( command == 2 )
		wait = true;
	if ( command == 8 ) {
		std::string launcherName = collectionItem->collectionInfo->launcher;
		std::string launcherFile = Utils::combinePath( Configuration::absolutePath, "collections", collectionItem->collectionInfo->name, "launchers", collectionItem->name + ".conf" );
		if (std::ifstream launcherStream( launcherFile.c_str( ) ); launcherStream.good( )) // Launcher file found
		{
			std::string line;
			if (std::getline( launcherStream, line)) // Launcher found
			{
				launcherName = line;
			}
		}
		launcherName = Utils::toLower( launcherName );
		std::string emulator = collection;
		config_.getProperty("launchers." + launcherName + ".LEDBlinkyEmulator", emulator );
		args = args + " \"" + emulator + "\"";
	}
	if ( command == 3 || command == 9 ) {
		std::string launcherName = collectionItem->collectionInfo->launcher;
		std::string launcherFile = Utils::combinePath( Configuration::absolutePath, "collections", collectionItem->collectionInfo->name, "launchers", collectionItem->name + ".conf" );
		if (std::ifstream launcherStream( launcherFile.c_str( ) ); launcherStream.good( )) // Launcher file found
		{
			std::string line;
			if (std::getline( launcherStream, line)) // Launcher found
			{
				launcherName = line;
			}
		}
		launcherName = Utils::toLower( launcherName );
		std::string emulator = launcherName;
		config_.getProperty("launchers." + launcherName + ".LEDBlinkyEmulator", emulator );
		args = args + " \"" + collectionItem->name + "\" \"" + emulator + "\"";
		if ( emulator == "" )
			return;
	}
	if ( LEDBlinkyDirectory != "" && !simpleExecute( exe, args, LEDBlinkyDirectory, wait )) {
        LOG_WARNING("LEDBlinky", "Failed to launch." );
	}
	return;
}

bool Launcher::simpleExecute(std::string executable, std::string args, std::string currentDirectory, bool wait, Page* currentPage)
{
    bool retVal = false;
    std::string executionString = "\"" + executable + "\" " + args;

    LOG_INFO("Launcher", "Attempting to launch: " + executionString);
    LOG_INFO("Launcher", "     from within folder: " + currentDirectory);

#ifdef WIN32
    STARTUPINFO startupInfo;
    PROCESS_INFORMATION processInfo;
    char applicationName[2048];
    char currDir[2048];
    memset(&applicationName, 0, sizeof(applicationName));
    memset(&startupInfo, 0, sizeof(startupInfo));
    memset(&processInfo, 0, sizeof(processInfo));
    strncpy(applicationName, executionString.c_str(), sizeof(applicationName));
    strncpy(currDir, currentDirectory.c_str(), sizeof(currDir));
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.wShowWindow = SW_SHOWDEFAULT;

    if (!CreateProcess(nullptr, applicationName, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, currDir, &startupInfo, &processInfo))
#else
    const std::size_t last_slash_idx = executable.rfind(Utils::pathSeparator);
    if (last_slash_idx != std::string::npos) {
        std::string applicationName = executable.substr(last_slash_idx + 1);
        executionString = "cd \"" + currentDirectory + "\" && exec \"./" + applicationName + "\" " + args;
    }
    if (system(executionString.c_str()) != 0)
#endif
    {
        LOG_WARNING("Launcher", "Failed to run: " + executable);
    }

    else
    {
#ifdef WIN32
        if (wait) {
            while (WAIT_OBJECT_0 != MsgWaitForMultipleObjects(1, &processInfo.hProcess, FALSE, INFINITE, QS_ALLINPUT)) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessage(&msg);
                }
            }
        }

        // result = GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
#endif
        retVal = true;
    }

    LOG_INFO("Launcher", "Completed");

    return retVal;
}

bool Launcher::execute(std::string executable, std::string args, std::string currentDirectory, bool wait, Page* currentPage, bool isAttractMode, Item* collectionItem)
{
    bool retVal = false;
    std::string executionString = "\"" + executable + "\" " + args;

    LOG_INFO("Launcher", "Attempting to launch: " + executionString);
    LOG_INFO("Launcher", "     from within folder: " + currentDirectory);

    std::atomic<bool> stop_thread = true;
    std::thread proc_thread;
    bool multiple_display = SDL_GetNumVideoDisplays() > 1;
    bool animateDuringGame = true;
    config_.getProperty("OPTION_ANIMATEDURINGGAME", animateDuringGame);
    
    if (animateDuringGame && multiple_display && currentPage != nullptr) {
        stop_thread = false;
        proc_thread = std::thread([this, &stop_thread, currentPage]() {
            this->keepRendering(std::ref(stop_thread), *currentPage);
            });
    }

#ifdef WIN32

    // Ensure executable and currentDirectory are absolute paths
    std::filesystem::current_path(Configuration::absolutePath);
    std::filesystem::path exePath(executable);
    if (!exePath.is_absolute()) {
        exePath = std::filesystem::absolute(exePath);
    }

    std::filesystem::path currDir(currentDirectory);
    if (!currDir.is_absolute()) {
        currDir = std::filesystem::absolute(currDir);
    }

    LOG_DEBUG("LauncherDebug", "Final executablePath: " + exePath.string());
    LOG_DEBUG("LauncherDebug", "Final currentDirectory: " + currDir.string());

    std::wstring exePathW = exePath.wstring();
    std::wstring argsW = std::wstring(args.begin(), args.end());
    std::wstring currDirW = currDir.wstring();

    // Lambda function to check if a window is fullscreen
    auto isFullscreenWindow = [](HWND hwnd) -> auto {
        RECT appBounds;
        if (!GetWindowRect(hwnd, &appBounds))
            return false;

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (hMonitor == nullptr)
            return false;

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfo(hMonitor, &monitorInfo))
            return false;

        return (appBounds.bottom - appBounds.top) == (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top)
            && (appBounds.right - appBounds.left) == (monitorInfo.rcMonitor.right - appBounds.left);
        };

    std::wstring executionStringW = exePathW + L" " + argsW;
    PWSTR pszApplication = nullptr;
    PWSTR pszCommandLine = nullptr;

    HRESULT hr = SHEvaluateSystemCommandTemplate(executionStringW.c_str(), &pszApplication, &pszCommandLine, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("Launcher", "Invalid execution string error: " + std::to_string(hr));
        return false;
    }

    LOG_INFO("Launcher", "Launching: " + Utils::wstringToString(executionStringW));

    // Lower priority before launching the process
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    HANDLE hLaunchedProcess = nullptr;
    bool handleObtained = false;

    if (exePath.extension() == L".exe") {
        STARTUPINFOW startupInfo;
        PROCESS_INFORMATION processInfo;
        memset(&startupInfo, 0, sizeof(startupInfo));
        memset(&processInfo, 0, sizeof(processInfo));
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startupInfo.wShowWindow = SW_SHOWDEFAULT;

        if (CreateProcessW(pszApplication, pszCommandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, currDirW.c_str(), &startupInfo, &processInfo)) {
            hLaunchedProcess = processInfo.hProcess;
            handleObtained = true;
            CloseHandle(processInfo.hThread);
        } else {
            LOG_WARNING("Launcher", "Failed to run: " + executable + " with error code: " + std::to_string(GetLastError()));
        }
    } else {
        SHELLEXECUTEINFOW shExInfo = {0};
        shExInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
        shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
        shExInfo.hwnd = nullptr;
        shExInfo.lpVerb = L"open";
        shExInfo.lpFile = pszApplication;
        shExInfo.lpParameters = pszCommandLine;
        shExInfo.lpDirectory = currDirW.c_str();
        shExInfo.nShow = SW_SHOWNORMAL;
        shExInfo.hInstApp = nullptr;

        if (ShellExecuteExW(&shExInfo)) {
            hLaunchedProcess = shExInfo.hProcess;
            if (hLaunchedProcess)
                handleObtained = true;
        } else {
            LOG_WARNING("Launcher", "ShellExecuteEx failed to run: " + executable + " with error code: " + std::to_string(GetLastError()));
        }
    }

    if (!handleObtained) {
        auto start = std::chrono::high_resolution_clock::now();
        HWND hwndFullscreen = nullptr;

        while (true) {
            HWND hwnd = GetForegroundWindow();
            if (hwnd != nullptr) {
                DWORD windowProcessId;
                GetWindowThreadProcessId(hwnd, &windowProcessId);
                if (windowProcessId != GetCurrentProcessId() && isFullscreenWindow(hwnd)) {
                    hwndFullscreen = hwnd;
                    break;
                }
            }

            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

            if (elapsed > 40000) {
                LOG_WARNING("Launcher", "Timeout while waiting for fullscreen state.");
                break;
            }

            Sleep(500);
        }

        if (hwndFullscreen) {
            DWORD gameProcessId;
            GetWindowThreadProcessId(hwndFullscreen, &gameProcessId);
            hLaunchedProcess = OpenProcess(SYNCHRONIZE, FALSE, gameProcessId);
            if (hLaunchedProcess) {
                handleObtained = true;
            }
        }
    }

    if (handleObtained) {
        bool is4waySet = false;
        bool isServoStikEnabled = false;
        config_.getProperty(OPTION_SERVOSTIKENABLED, isServoStikEnabled);
        if (currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos && isServoStikEnabled) {
            if (!PacSetServoStik4Way()) {
                LOG_ERROR("RetroFE", "Failed to set ServoStik to 4-way mode");
            }
            else {
                LOG_INFO("RetroFE", "Setting ServoStik to 4-way mode");
                is4waySet = true;
            }
        }
        if (isAttractMode) {
            int attractModeLaunchRunTime = 30;
            config_.getProperty(OPTION_ATTRACTMODELAUNCHRUNTIME, attractModeLaunchRunTime);
            auto start = std::chrono::high_resolution_clock::now();
            bool userInputDetected = false;

            auto isAnyKeyPressed = []() -> auto {
                // Common virtual key codes, adjust this list as necessary
                std::vector<int> relevantKeys = {
                    // Letters A-Z
                    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
                    0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54,
                    0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, // A-Z
                    // Numbers 0-9
                    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, // 0-9
                    // Arrow keys
                    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                    // Other common keys
                    VK_RETURN, VK_SPACE, VK_ESCAPE, VK_TAB, VK_BACK,
                    // Modifier keys
                    VK_SHIFT, VK_CONTROL, VK_MENU // Shift, Ctrl, Alt
                };

                for (int virtualKey : relevantKeys) {
                    if (GetAsyncKeyState(virtualKey) & 0x8000) {
                        LOG_INFO("Launcher", "Key press detected: " + std::to_string(virtualKey));
                        return true;
                    }
                }
                return false;
                };

            auto isAnyControllerButtonPressed = []() -> auto {
                XINPUT_STATE state;
                ZeroMemory(&state, sizeof(XINPUT_STATE));
                if (XInputGetState(0, &state) == ERROR_SUCCESS) { // Check the first controller
                    if (state.Gamepad.wButtons != 0) { // Any button is pressed
                        return true;
                    }
                }
                return false;
                };
            
            while (true) {
             
                // Process window messages
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessage(&msg);
                }

                // Check for keyboard/controller input
                if (isAnyKeyPressed() || isAnyControllerButtonPressed()) {
                    LOG_INFO("Launcher", "User input detected, waiting indefinitely.");
                    userInputDetected = true;
                    break;
                }

                // Check if the launched process has exited
                DWORD exitCode;
                if (GetExitCodeProcess(hLaunchedProcess, &exitCode)) {
                    if (exitCode != STILL_ACTIVE) {
                        // Process has terminated
                        break;
                    }
                }

                // Check if the timeout has been reached
                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
                if (elapsed >= attractModeLaunchRunTime) {
                    break;
                }

                // Sleep to prevent high CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (userInputDetected) {
                while(WAIT_OBJECT_0 != MsgWaitForMultipleObjects(1, &hLaunchedProcess, FALSE, INFINITE, QS_ALLINPUT)) {
                    MSG msg;
                    while(PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        DispatchMessage(&msg);
                    }
                    // Add to last played if user interrupted during attract mode
                    CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
                    std::string lastPlayedSkipCollection = "";
                    int size = 0;
                    config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
                    config_.getProperty(OPTION_LASTPLAYEDSIZE, size);

                    if (lastPlayedSkipCollection != "")
                    {
                        // see if any of the comma seperated match current collection
                        std::stringstream ss(lastPlayedSkipCollection);
                        std::string collection = "";
                        bool updateLastPlayed = true;
                        while (ss.good())
                        {
                            getline(ss, collection, ',');
                            // Check if the current collection matches any collection in lastPlayedSkipCollection
                            if (collectionItem->collectionInfo->name == collection)
                            {
                                updateLastPlayed = false;
                                break; // No need to check further, as we found a match
                            }
                        }
                        // Update last played collection if not found in the skip collection
                        if (updateLastPlayed)
                        {
                            cib.updateLastPlayedPlaylist(currentPage->getCollection(), collectionItem, size);
                            //currentPage_->updateReloadables(0);
                        }
                    }
                }
            } else {
                LOG_INFO("Launcher", "Attract Mode timeout reached, terminating game.");
                DWORD processId = GetProcessId(hLaunchedProcess);
                TerminateProcessAndChildren(processId);  // Terminate the process and its children
            }
        }
        else if (wait) {
            while (WAIT_OBJECT_0 != MsgWaitForMultipleObjects(1, &hLaunchedProcess, FALSE, INFINITE, QS_ALLINPUT)) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessage(&msg);
                }
            }

        }
        if (is4waySet)
        {
            if (!PacSetServoStik8Way()) { // return to 8-way if 4-way switch occurred
                LOG_ERROR("RetroFE", "Failed to return ServoStik to 8-way mode");
            }
            else {
                LOG_INFO("RetroFE", "Returned ServoStik to 8-way mode");
            }
        }
        CloseHandle(hLaunchedProcess);
        retVal = true;
    }

    bool highPriority = false;
    config_.getProperty("OPTION_HIGHPRIORITY", highPriority);
    SetPriorityClass(GetCurrentProcess(), highPriority ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);

    if (pszApplication) {
        CoTaskMemFree(pszApplication);
    }
    if (pszCommandLine) {
        CoTaskMemFree(pszCommandLine);
    }

#else
    const std::size_t last_slash_idx = executable.rfind(Utils::pathSeparator);
    if (last_slash_idx != std::string::npos) {
        std::string applicationName = executable.substr(last_slash_idx + 1);
        executionString = "cd \"" + currentDirectory + "\" && exec \"./" + applicationName + "\" " + args;
    }
    if (system(executionString.c_str()) != 0) {
        LOG_WARNING("Launcher", "Failed to run: " + executable);
    } else {
        retVal = true;
    }

    if (isAttractMode) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        LOG_INFO("Launcher", "Attract Mode timeout reached.");
        // Unix/Linux specific logic to terminate the process if needed
    }
#endif

    if (multiple_display && stop_thread == false) {
        stop_thread = true;
        proc_thread.join();
    }

    LOG_INFO("Launcher", "Completed");

    return retVal;
    }



void Launcher::keepRendering(std::atomic<bool> &stop_thread, Page &currentPage) const
{
    float lastTime = 0;
    float currentTime = 0;
    float deltaTime = 0;
    double sleepTime;
    double fpsTime = 1000.0 / static_cast<double>(60);

    while (!stop_thread) {
        lastTime = currentTime;
        currentTime = static_cast<float>(SDL_GetTicks()) / 1000;

        if (currentTime < lastTime) {
            currentTime = lastTime;
        }

        deltaTime = currentTime - lastTime;
        sleepTime = fpsTime - deltaTime * 1000;
            
        if (sleepTime > 0 && sleepTime < 1000) {
            SDL_Delay(static_cast<unsigned int>(sleepTime));
        }
        currentPage.update(float(0));
        SDL_LockMutex(SDL::getMutex());

        // start on secondary monitor
        // todo support future main screen swap
        for (int i = 1; i < SDL::getScreenCount(); ++i) {
            SDL_SetRenderTarget(SDL::getRenderer(i), SDL::getRenderTarget(i));
            SDL_SetRenderDrawColor(SDL::getRenderer(i), 0x0, 0x0, 0x0, 0xFF);
            SDL_RenderClear(SDL::getRenderer(i));
        }

        currentPage.draw();

        for (int i = 1; i < SDL::getScreenCount(); ++i) {
            // Switch back to the screen's framebuffer
            SDL_SetRenderTarget(SDL::getRenderer(i), nullptr);

            // Render the texture onto the screen
            SDL_RenderCopy(SDL::getRenderer(i), SDL::getRenderTarget(i), nullptr, nullptr);

            // Present the final result to the screen
            SDL_RenderPresent(SDL::getRenderer(i));
        }

        SDL_UnlockMutex(SDL::getMutex());
    }
}

bool Launcher::launcherName(std::string &launcherName, std::string collection)
{
       // find the launcher for the particular item 
    if (std::string launcherKey = "collections." + collection + ".launcher"; !config_.getProperty(launcherKey, launcherName)) {
        std::stringstream ss;

        ss << "Launch failed. Could not find a configured launcher for collection \""
           << collection
           << "\" (could not find a property for \""
           << launcherKey
           << "\")";

        LOG_ERROR("Launcher", ss.str());

        return false;
    }

    std::stringstream ss;
    ss        << "collections."
              << collection
              << " is configured to use launchers."
              << launcherName
              << "\"";

    LOG_DEBUG("Launcher", ss.str());

    return true;
}



bool Launcher::launcherExecutable(std::string& executable, std::string launcherName) {
    // Try with the localLauncher prefix
    std::string executableKey = "localLaunchers." + launcherName + ".executable";
    if (!config_.getProperty(executableKey, executable)) {
        // Try with the collectionLauncher prefix
        executableKey = "collectionLaunchers." + launcherName + ".executable";
        if (!config_.getProperty(executableKey, executable)) {
            // Finally, try with the global launcher prefix
            executableKey = "launchers." + launcherName + ".executable";
            if (!config_.getProperty(executableKey, executable)) {
                LOG_ERROR("Launcher", "No launcher found for: " + executableKey);
                return false;
            }
        }
    }
    return true;
}



bool Launcher::launcherArgs(std::string& args, std::string launcherName) {
    // Try with the localLauncher prefix
    std::string argsKey = "localLaunchers." + launcherName + ".arguments";
    if (!config_.getProperty(argsKey, args)) {
        // Try with the collectionLauncher prefix
        argsKey = "collectionLaunchers." + launcherName + ".arguments";
        if (!config_.getProperty(argsKey, args)) {
            // Finally, try with the global launcher prefix
            argsKey = "launchers." + launcherName + ".arguments";
            if (!config_.getProperty(argsKey, args)) {
                LOG_WARNING("Launcher", "No arguments specified for: " + argsKey);
                args.clear(); // Ensure args is empty if not found
            }
        }
    }
    return true;
}



bool Launcher::extensions(std::string &extensions, std::string collection)
{
    if(std::string extensionsKey = "collections." + collection + ".list.extensions"; !config_.getProperty(extensionsKey, extensions)) {
        LOG_ERROR("Launcher", "No extensions specified for: " + extensionsKey);
        return false;
    }

    extensions = Utils::replace(extensions, " ", "");
    extensions = Utils::replace(extensions, ".", "");

    return true;
}

bool Launcher::collectionDirectory(std::string &directory, std::string collection)
{
    std::string itemsPathValue;
    std::string mergedCollectionName;

    // find the items path folder (i.e. ROM path)
    config_.getCollectionAbsolutePath(collection, itemsPathValue);
    directory += itemsPathValue + Utils::pathSeparator;

    return true;
}

bool Launcher::findFile(std::string& foundFilePath, std::string& foundFilename, const std::string& directory, const std::string& filenameWithoutExtension, const std::string& extensions) {
    bool fileFound = false;
    std::stringstream ss(extensions);
    std::string extension;

    while (!fileFound && std::getline(ss, extension, ',')) {
        fs::path filePath = fs::path(directory) / (filenameWithoutExtension + "." + extension);

        if (fs::exists(filePath)) {
            foundFilePath = fs::absolute(filePath).string();
            foundFilename = extension;
            fileFound = true;
            LOG_INFO("Launcher", "File found: " + foundFilePath);
        }
        else {
            LOG_WARNING("Launcher", "File not found: " + filePath.string());
        }
    }

    if (!fileFound) {
        LOG_WARNING("Launcher", "Could not find any files with the name \"" + filenameWithoutExtension + "\" in folder \"" + directory + "\"");
    }

    return fileFound;
}

