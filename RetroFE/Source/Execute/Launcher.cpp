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

 // --- Core Required Headers ---
#include "Launcher.h"
#include "../Collection/CollectionInfoBuilder.h"
#include "../Collection/Item.h"
#include "../Database/Configuration.h"
#include "../Database/GlobalOpts.h"
#include "../Database/HiScores.h"
#include "../Graphics/Page.h"
#include "../RetroFE.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"

// --- C++ Standard Library ---
#include <chrono>
#include <cstdlib> // For getenv
#include <filesystem>
#include <fstream>
#include <memory>  // For std::unique_ptr
#include <optional>
#include <sstream>

// --- New Refactored Components ---
#include "Input/InputMonitor.h"
#include "Platform/IProcessManager.h"
#include "Util/RestrictorGuard.h"

// --- Concrete Implementations (Platform-Specific) ---
#ifdef WIN32
#include "Platform/Windows/WindowsProcessManager.h"
#else
#include "Platform/Unix/UnixProcessManager.h"
#endif


namespace fs = std::filesystem;

Launcher::Launcher(Configuration& c, RetroFE& retroFe)
	: config_(c),
	retroFeInstance_(retroFe) {
}

static std::string replaceVariables(std::string str,
	const std::string& itemFilePath,
	const std::string& itemName,
	const std::string& itemFilename,
	const std::string& itemDirectory,
	const std::string& itemCollectionName) {
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

bool Launcher::run(std::string collection, Item* collectionItem, Page* currentPage, bool isAttractMode) {
    //
    // --- STEP 1-5: GATHER AND PREPARE ALL LAUNCH PARAMETERS ---
    //
    std::string launcherName = collectionItem->collectionInfo->launcher;
    std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collection, "launchers", collectionItem->name + ".conf");

    // Per-item launcher override file
    if (std::ifstream launcherStream(launcherFile); launcherStream.good()) {
        std::string line;
        if (std::getline(launcherStream, line)) {
            std::string localLauncherKey = "localLaunchers." + collection + "." + line;
            launcherName = config_.propertyPrefixExists(localLauncherKey) ? (collection + "." + line) : line;
            LOG_INFO("Launcher", "Using per-item launcher override: " + launcherName);
        }
    }

    // Collection-specific launcher fallback
    if (launcherName == collectionItem->collectionInfo->launcher) {
        std::string collectionLauncherKey = "collectionLaunchers." + collection;
        if (config_.propertyPrefixExists(collectionLauncherKey)) {
            launcherName = collectionItem->collectionInfo->name;
            LOG_INFO("Launcher", "Using collection-specific launcher: " + launcherName);
        }
    }

    // Lambda helpers: property fallback chain (local -> collection -> global)
    auto getPropChainStr = [&](const std::string& leaf, std::string& out) -> bool {
        if (config_.getProperty("localLaunchers." + launcherName + "." + leaf, out)) return true;
        if (config_.getProperty("collectionLaunchers." + launcherName + "." + leaf, out)) return true;
        if (config_.getProperty("launchers." + launcherName + "." + leaf, out)) return true;
        return false;
        };
    auto getPropChainBool = [&](const std::string& leaf, bool& out) -> bool {
        if (config_.getProperty("localLaunchers." + launcherName + "." + leaf, out)) return true;
        if (config_.getProperty("collectionLaunchers." + launcherName + "." + leaf, out)) return true;
        if (config_.getProperty("launchers." + launcherName + "." + leaf, out)) return true;
        return false;
        };

    std::string executablePath, selectedItemsDirectory, selectedItemsPath, extensionstr, matchedExtension, args;

    if (!getPropChainStr("executable", executablePath)) {
        LOG_ERROR("Launcher", "Launcher executable not found for: " + launcherName);
        return false;
    }
    if (!extensions(extensionstr, collection)) {
        LOG_ERROR("Launcher", "No file extensions configured for collection: " + collection);
        return false;
    }
    if (!collectionDirectory(selectedItemsDirectory, collection)) {
        LOG_ERROR("Launcher", "No valid directory found for collection: " + collection);
        return false;
    }

    getPropChainStr("arguments", args);

    if (!collectionItem->filepath.empty()) {
        selectedItemsDirectory = collectionItem->filepath;
        LOG_DEBUG("Launcher", "Using filepath from item: " + selectedItemsDirectory);
    }

    if (collectionItem->file.empty()) {
        findFile(selectedItemsPath, matchedExtension, selectedItemsDirectory, collectionItem->name, extensionstr);
    }
    else {
        findFile(selectedItemsPath, matchedExtension, selectedItemsDirectory, collectionItem->file, extensionstr);
    }

    // 1) Variable replacement (exe + args)
    LOG_DEBUG("Launcher", "Path before replacement: " + executablePath);
    args = replaceVariables(args, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
    executablePath = replaceVariables(executablePath, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
    LOG_INFO("Launcher", "Path after variable replacement: " + executablePath);

    // 2) Absolutize executable path (relative to RetroFE root if needed)
    std::filesystem::path finalExePath(executablePath);
    if (!finalExePath.is_absolute()) {
        finalExePath = std::filesystem::path(Configuration::absolutePath) / executablePath;
        executablePath = finalExePath.string();
        LOG_INFO("Launcher", "Resolved relative executable path to: " + executablePath);
    }

    // 3) Determine working directory (default = dir of exe), then fallback chain, then variable replacement
    std::string currentDirectory = Utils::getDirectory(executablePath); // Start with a sensible default
    getPropChainStr("currentDirectory", currentDirectory); // The helper will overwrite 'currentDirectory' if the property is found
    currentDirectory = replaceVariables(currentDirectory, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);

    // Flags
    bool reboot = false;
    getPropChainBool("reboot", reboot);

    bool unloadSDL = false;
    config_.getProperty(OPTION_UNLOADSDL, unloadSDL);

    // Build onFrameTick once (reused for pre wait, main wait, and optionally post wait)
    FrameTickCallback onFrameTick;
    if (unloadSDL) {
        auto start = std::chrono::steady_clock::now();
        onFrameTick = [this, currentPage, t = start]() mutable {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - t).count();
            t = now;
            if (dt > 0.1f) dt = 0.0167f;
            if (currentPage) currentPage->update(dt);
            };
    }
    else {
        Uint64 start = SDL_GetPerformanceCounter();
        onFrameTick = [this, currentPage, last = start]() mutable {
            Uint64 now = SDL_GetPerformanceCounter();
            double f = static_cast<double>(SDL_GetPerformanceFrequency());
            float  dt = static_cast<float>((now - last) / f);
            last = now;
            if (dt > 0.1f) dt = 0.0167f;

            if (!currentPage) return;
            currentPage->update(dt);

            bool multiple_display = SDL::getScreenCount() > 1;
            bool animateDuringGame = true;
            config_.getProperty(OPTION_ANIMATEDURINGGAME, animateDuringGame);
            if (animateDuringGame && multiple_display) {
                for (int i = 1; i < SDL::getScreenCount(); ++i) {
                    SDL_Renderer* r = SDL::getRenderer(i);
                    SDL_Texture* t = SDL::getRenderTarget(i);
                    if (!r || !t) continue;
                    SDL_SetRenderTarget(r, t);
                    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
                    SDL_RenderClear(r);
                    currentPage->draw(i);
                    SDL_SetRenderTarget(r, nullptr);
                    SDL_RenderCopy(r, t, nullptr, nullptr);
                    SDL_RenderPresent(r);
                }
            }
            };
    }

    // --- PRE HOOK (sequential by default; uses fallback chain; UI animates while waiting) ---
// --- PRE HOOK (robust, sequential by default, UI animates while waiting) ---
    {
        std::string preExe;
        if (getPropChainStr("preexecutable", preExe)) {
            // --- A pre-hook is configured, so prepare and attempt to run it ---
            std::string preArgs, preCwd;
            bool preWait = true;

            // Gather the rest of the hook's properties
            getPropChainStr("prearguments", preArgs);
            getPropChainStr("precurrentDirectory", preCwd);
            getPropChainBool("prewait", preWait);

            // Process all variables and paths
            preExe = replaceVariables(preExe, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
            preArgs = replaceVariables(preArgs, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
            if (preCwd.empty()) {
                preCwd = Utils::getDirectory(preExe); // Default CWD to the executable's directory
            }
            preCwd = replaceVariables(preCwd, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);

            // Resolve paths to be absolute for reliability
            std::filesystem::path pExe(preExe), pCwd(preCwd);
            if (!pExe.is_absolute()) pExe = std::filesystem::path(Configuration::absolutePath) / pExe;
            if (!pCwd.is_absolute()) pCwd = std::filesystem::path(Configuration::absolutePath) / pCwd;

            // --- CORE REFACTOR: Check for existence BEFORE launching ---
            if (!std::filesystem::exists(pExe)) {
                LOG_WARNING("Launcher", "Pre-hook executable not found, skipping: " + pExe.string());
            }
            else {
                // The file exists, so we expect it to run. A failure here is a real error.
                std::unique_ptr<IProcessManager> preMgr;
#ifdef WIN32
                preMgr = std::make_unique<WindowsProcessManager>();
#else
                preMgr = std::make_unique<UnixProcessManager>();
#endif
                if (!preMgr->launch(pExe.string(), preArgs, pCwd.string())) {
                    LOG_ERROR("Launcher", "Pre-hook failed to start even though it exists: " + pExe.string());
                    return false; // Critical failure, halt the launch
                }

                if (preWait) {
                    LOG_INFO("Launcher", "Waiting for pre-hook process to complete...");
                    preMgr->wait(0, nullptr, onFrameTick);
                    LOG_INFO("Launcher", "Pre-hook complete.");
                }
                else {
                    LOG_INFO("Launcher", "Pre-hook started in fire-and-forget mode.");
                }
            }
        }
        else {
            LOG_DEBUG("Launcher", "No preexecutable configured; skipping pre hook.");
        }
    }
    //
    // --- STEP 6: EXECUTION ---
    //

    // 6a) Create the platform-specific process manager
    std::unique_ptr<IProcessManager> processManager;
#ifdef WIN32
    processManager = std::make_unique<WindowsProcessManager>();
#else
    processManager = std::make_unique<UnixProcessManager>();
#endif

    // 6c) Launch the main process
    if (!processManager->launch(executablePath, args, currentDirectory)) {
        LOG_ERROR("Launcher", "Execution failed for: " + executablePath);
        return false;
    }

    // --- Wait/monitor logic ---
    if (reboot) {
        LOG_INFO("Launcher", "Reboot mode enabled. Entering simple wait until process terminates.");
        processManager->wait(0, nullptr, nullptr);  // minimal wait; no UI tick
    }
    else {
        LOG_INFO("Launcher", "Normal mode. Entering monitoring state.");

        // 6b) Helpers
        InputMonitor inputMonitor(config_);
        std::optional<RestrictorGuard> restrictorGuard;

        bool restrictorEnabled = false;
        config_.getProperty("restrictorEnabled", restrictorEnabled);
        if (!isAttractMode && restrictorEnabled &&
            currentPage && currentPage->getSelectedItem() &&
            currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos) {
            restrictorGuard.emplace(4);
        }

        // 6d) Monitor the process
        auto startTime = std::chrono::steady_clock::now();
        auto endTime = startTime;
        auto interruptionTime = startTime;
        bool userInputDetected = false;

        if (isAttractMode) {
            int timeout = 30;
            config_.getProperty(OPTION_ATTRACTMODELAUNCHRUNTIME, timeout);
            auto attractModeInputCheck = [&inputMonitor]() {
                return inputMonitor.checkInputEvents() != InputDetectionResult::NoInput;
                };
            WaitResult result = processManager->wait(timeout, attractModeInputCheck, onFrameTick);

            if (result == WaitResult::UserInput) {
                userInputDetected = true;
                interruptionTime = std::chrono::steady_clock::now();

                if (inputMonitor.wasQuitFirstInput()) {
                    LOG_INFO("Launcher", "User interrupted attract mode with QUIT command. Terminating.");
                    processManager->terminate();
                }
                else {
                    LOG_INFO("Launcher", "User interrupted attract mode with PLAY command. Waiting for game to exit naturally.");
                    if (restrictorEnabled && currentPage && currentPage->getSelectedItem() &&
                        currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos) {
                        LOG_INFO("Launcher", "User taking over 4-way game in attract mode. Engaging restrictor.");
                        restrictorGuard.emplace(4);
                    }
                    CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
                    int lastPlayedSize = 10;
                    config_.getProperty(OPTION_LASTPLAYEDSIZE, lastPlayedSize);
                    cib.updateLastPlayedPlaylist(currentPage->getCollection(), collectionItem, lastPlayedSize);

                    auto quitCheck = [&inputMonitor]() { return inputMonitor.checkInputEvents() == InputDetectionResult::QuitInput; };
                    processManager->wait(0, quitCheck, onFrameTick);
                    processManager->terminate();
                }
            }
            else if (result == WaitResult::Timeout) {
                LOG_INFO("Launcher", "Attract mode timeout reached. Terminating process.");
                processManager->terminate();
            }
            endTime = std::chrono::steady_clock::now();
        }
        else { // Normal mode
            LOG_INFO("Launcher", "Waiting for launched process to complete. Press quit combo to force quit.");
            auto quitCheck = [&inputMonitor]() { return inputMonitor.checkInputEvents() == InputDetectionResult::QuitInput; };
            WaitResult result = processManager->wait(0, quitCheck, onFrameTick);

            if (result == WaitResult::UserInput) {
                LOG_INFO("Launcher", "User pressed quit combo during game. Terminating process.");
                processManager->terminate();
            }
            else {
                LOG_INFO("Launcher", "Process completed naturally.");
            }
            endTime = std::chrono::steady_clock::now();
        }

        // 6e) Update stats (time tracking)
        double gameplayDuration = 0.0;
        bool trackTime = false;
        bool shouldRunHi2Txt = false;

        if (!isAttractMode) {
            if (!inputMonitor.wasQuitFirstInput()) {
                trackTime = true;
                gameplayDuration = std::chrono::duration<double>(endTime - startTime).count();
                shouldRunHi2Txt = true;
            }
            else {
                LOG_INFO("Launcher", "Immediate quit combo detected; not tracking gameplay time.");
            }
        }
        else if (userInputDetected) {
            if (!inputMonitor.wasQuitFirstInput()) {
                trackTime = true;
                gameplayDuration = std::chrono::duration<double>(endTime - interruptionTime).count();
                shouldRunHi2Txt = true;
                LOG_INFO("Launcher", "Attract mode interrupted to play; tracking gameplay time.");
            }
            else {
                LOG_INFO("Launcher", "Attract mode interrupted with immediate quit; not tracking time.");
            }
        }

        if (trackTime && gameplayDuration > 0) {
            LOG_INFO("Launcher", "Gameplay time recorded: " + std::to_string(gameplayDuration) + " seconds.");
            CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
            cib.updateTimeSpent(collectionItem, gameplayDuration);
        }

        if (shouldRunHi2Txt && executablePath.find("mame") != std::string::npos && collectionItem != nullptr) {
            HiScores::getInstance().runHi2Txt(collectionItem->name);
        }
    }

    //
    // --- STEP 7: REBOOT CHECK ---
    //

    // --- POST HOOK (fire-and-forget by default; fallback chain) ---
    {
        std::string postExe, postArgs, postCwd;
        bool postWait = false;

        bool havePost = getPropChainStr("postexecutable", postExe);
        if (havePost) {
            (void)getPropChainStr("postarguments", postArgs);
            (void)getPropChainStr("postcurrentDirectory", postCwd);
            (void)getPropChainBool("postwait", postWait);

            postExe = replaceVariables(postExe, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
            postArgs = replaceVariables(postArgs, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
            if (postCwd.empty()) postCwd = Utils::getDirectory(postExe);
            postCwd = replaceVariables(postCwd, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);

            std::filesystem::path pExe(postExe), pCwd(postCwd);
            if (!pExe.is_absolute()) pExe = std::filesystem::path(Configuration::absolutePath) / pExe;
            if (!pCwd.is_absolute()) pCwd = std::filesystem::path(Configuration::absolutePath) / pCwd;

            std::unique_ptr<IProcessManager> postMgr;
#ifdef WIN32
            postMgr = std::make_unique<WindowsProcessManager>();
#else
            postMgr = std::make_unique<UnixProcessManager>();
#endif

            if (postWait) {
                if (postMgr->launch(pExe.string(), postArgs, pCwd.string())) {
                    postMgr->wait(0, nullptr, onFrameTick); // animate UI if desired
                    LOG_INFO("Launcher", "Post hook complete.");
                }
                else {
                    LOG_WARNING("Launcher", "Post hook failed to start: " + pExe.string());
                }
            }
            else {
                if (!postMgr->simpleLaunch(pExe.string(), postArgs, pCwd.string())) {
                    LOG_WARNING("Launcher", "Post hook failed to start: " + pExe.string());
                }
                else {
                    LOG_INFO("Launcher", "Post hook started: " + pExe.string());
                }
            }
        }
        else {
            LOG_DEBUG("Launcher", "No postexecutable configured; skipping post hook.");
        }
    }

    LOG_INFO("Launcher", "Execution completed for: " + executablePath + " with reboot flag: " + std::to_string(reboot));
    return reboot;
}

bool Launcher::runHookNoWait_(const std::string& exe, const std::string& args, const std::string& cwd) {
#ifdef WIN32
    auto pm = std::make_unique<WindowsProcessManager>();
#else
    auto pm = std::make_unique<UnixProcessManager>();
#endif
    return pm->simpleLaunch(exe, args, cwd);
}


void Launcher::startScript() {
#ifdef WIN32
	std::string exe = Utils::combinePath(Configuration::absolutePath, "start.bat");
#else
	std::string exe = Utils::combinePath(Configuration::absolutePath, "start.sh");
#endif
	if (fs::exists(exe)) {
		std::unique_ptr<IProcessManager> processManager;
#ifdef WIN32
		processManager = std::make_unique<WindowsProcessManager>();
#else
		processManager = std::make_unique<UnixProcessManager>();
#endif
		processManager->simpleLaunch(exe, "", Configuration::absolutePath);
	}
}

void Launcher::exitScript() {
#ifdef WIN32
	std::string exe = Utils::combinePath(Configuration::absolutePath, "exit.bat");
#else
	std::string exe = Utils::combinePath(Configuration::absolutePath, "exit.sh");
#endif
	if (fs::exists(exe)) {
		std::unique_ptr<IProcessManager> processManager;
#ifdef WIN32
		processManager = std::make_unique<WindowsProcessManager>();
#else
		processManager = std::make_unique<UnixProcessManager>();
#endif
		processManager->simpleLaunch(exe, "", Configuration::absolutePath);
	}
}

void Launcher::LEDBlinky(int command, std::string collection, Item* collectionItem) {
	// This entire feature is Windows-specific. On other platforms, this function is a no-op.
#ifdef WIN32
	std::string LEDBlinkyDirectory = "";
	config_.getProperty(OPTION_LEDBLINKYDIRECTORY, LEDBlinkyDirectory);
	if (LEDBlinkyDirectory.empty()) {
		return;
	}

	std::string exe = Utils::combinePath(LEDBlinkyDirectory, "LEDBlinky.exe");
	if (!std::filesystem::exists(exe)) {
		return;
	}

	std::string args = std::to_string(command);
	bool wait = (command == 2);

	if (command == 8) {
		std::string launcherName = collectionItem->collectionInfo->launcher;
		std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collectionItem->collectionInfo->name, "launchers", collectionItem->name + ".conf");
		if (std::ifstream launcherStream(launcherFile.c_str()); launcherStream.good())
		{
			std::string line;
			if (std::getline(launcherStream, line))
			{
				launcherName = line;
			}
		}
		launcherName = Utils::toLower(launcherName);
		std::string emulator = collection;
		config_.getProperty("launchers." + launcherName + ".LEDBlinkyEmulator", emulator);
		args = args + " \"" + emulator + "\"";
	}
	else if (command == 3 || command == 9) {
		std::string launcherName = collectionItem->collectionInfo->launcher;
		std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collectionItem->collectionInfo->name, "launchers", collectionItem->name + ".conf");
		if (std::ifstream launcherStream(launcherFile.c_str()); launcherStream.good())
		{
			std::string line;
			if (std::getline(launcherStream, line))
			{
				launcherName = line;
			}
		}
		launcherName = Utils::toLower(launcherName);
		std::string emulator = launcherName;
		config_.getProperty("launchers." + launcherName + ".LEDBlinkyEmulator", emulator);
		if (emulator.empty()) {
			return;
		}
		args = args + " \"" + collectionItem->name + "\" \"" + emulator + "\"";
	}

	// --- Execution Logic ---
	std::unique_ptr<IProcessManager> processManager = std::make_unique<WindowsProcessManager>();

	if (wait) {
		// Launch and wait for the process to finish.
		if (processManager->launch(exe, args, LEDBlinkyDirectory)) {
			// Wait indefinitely with no special checks for input or UI updates.
			processManager->wait(0, nullptr, nullptr);
		}
		else {
			LOG_WARNING("LEDBlinky", "Failed to launch (wait mode).");
		}
	}
	else {
		// Fire-and-forget.
		if (!processManager->simpleLaunch(exe, args, LEDBlinkyDirectory)) {
			LOG_WARNING("LEDBlinky", "Failed to launch (no-wait mode).");
		}
	}
#endif // WIN32
}

bool Launcher::launcherName(std::string& launcherName, std::string collection) {
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
	ss << "collections."
		<< collection
		<< " is configured to use launchers."
		<< launcherName
		<< "\"";

	LOG_DEBUG("Launcher", ss.str());

	return true;
}

bool Launcher::extensions(std::string& extensions, std::string collection) {
	if (std::string extensionsKey = "collections." + collection + ".list.extensions"; !config_.getProperty(extensionsKey, extensions)) {
		LOG_ERROR("Launcher", "No extensions specified for: " + extensionsKey);
		return false;
	}

	extensions = Utils::replace(extensions, " ", "");
	extensions = Utils::replace(extensions, ".", "");

	return true;
}

bool Launcher::collectionDirectory(std::string& directory, std::string collection) {
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

	while (std::getline(ss, extension, ',')) {
		fs::path filePath = fs::path(directory) / (filenameWithoutExtension + "." + extension);

		if (fs::exists(filePath)) {
			foundFilePath = fs::absolute(filePath).string();
			foundFilename = extension;
			fileFound = true;
			LOG_INFO("Launcher", "File found: " + foundFilePath + " with extension: ." + extension);
			break; // Exit the loop once the file is found
		}
	}

	if (!fileFound) {
		LOG_ERROR("Launcher", "No matching files found for \"" + filenameWithoutExtension + "\" in directory \"" + directory + "\" with extensions: " + extensions);
	}

	return fileFound;
}

