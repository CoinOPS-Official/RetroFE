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

    if (std::ifstream launcherStream(launcherFile); launcherStream.good()) {
        std::string line;
        if (std::getline(launcherStream, line)) {
            std::string localLauncherKey = "localLaunchers." + collection + "." + line;
            launcherName = config_.propertyPrefixExists(localLauncherKey) ? (collection + "." + line) : line;
            LOG_INFO("Launcher", "Using per-item launcher override: " + launcherName);
        }
    }

    if (launcherName == collectionItem->collectionInfo->launcher) {
        std::string collectionLauncherKey = "collectionLaunchers." + collection;
        if (config_.propertyPrefixExists(collectionLauncherKey)) {
            launcherName = collectionItem->collectionInfo->name;
            LOG_INFO("Launcher", "Using collection-specific launcher: " + launcherName);
        }
    }

    std::string executablePath, selectedItemsDirectory, selectedItemsPath, extensionstr, matchedExtension, args;

    if (!launcherExecutable(executablePath, launcherName)) {
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

    launcherArgs(args, launcherName);

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

    // 1. First, replace all variables. This is where %ITEM_FILEPATH% gets resolved.
    LOG_DEBUG("Launcher", "Path before replacement: " + executablePath);
    args = replaceVariables(args, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
    executablePath = replaceVariables(executablePath, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
    LOG_INFO("Launcher", "Path after variable replacement: " + executablePath);

    // 2. Now that we have a real path string, resolve it to an absolute path if it isn't already.
    std::filesystem::path finalExePath(executablePath);
    if (!finalExePath.is_absolute()) {
        finalExePath = std::filesystem::path(Configuration::absolutePath) / executablePath;
        executablePath = finalExePath.string();
        LOG_INFO("Launcher", "Resolved relative executable path to: " + executablePath);
    }

    // 3. Finally, determine the working directory based on the final, absolute executable path.
    std::string currentDirectoryKey = "launchers." + launcherName + ".currentDirectory";
    std::string currentDirectory = Utils::getDirectory(executablePath);
    config_.getProperty(currentDirectoryKey, currentDirectory);
    currentDirectory = replaceVariables(currentDirectory, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);

    //
    // --- STEP 6: EXECUTION ---
    //

    // 6a. Create the platform-specific process manager
    std::unique_ptr<IProcessManager> processManager;
#ifdef WIN32
    processManager = std::make_unique<WindowsProcessManager>();
#else
    processManager = std::make_unique<UnixProcessManager>();
#endif

    // 6b. Create helper components
    InputMonitor inputMonitor(config_);
    std::optional<RestrictorGuard> restrictorGuard;

    bool restrictorEnabled = false;
    config_.getProperty("restrictorEnabled", restrictorEnabled);
    if (!isAttractMode && restrictorEnabled && currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos) {
        restrictorGuard.emplace(4);
    }

    // 6c. Launch the process
    if (!processManager->launch(executablePath, args, currentDirectory)) {
        LOG_ERROR("Launcher", "Execution failed for: " + executablePath);
        return false;
    }

    // 6d. Monitor the process
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime;
    auto interruptionTime = startTime;
    bool userInputDetected = false;

    Uint64 lastTick = SDL_GetPerformanceCounter();

    auto onFrameTick = [this, currentPage, &lastTick]() {
        // Delta time calculation using SDL's high-resolution timer
        Uint64 now = SDL_GetPerformanceCounter();
        auto freq = static_cast<double>(SDL_GetPerformanceFrequency());
        auto delta = static_cast<float>((now - lastTick) / freq);
        lastTick = now;

        bool multiple_display = SDL::getScreenCount() > 1;
        bool animateDuringGame = true;
        config_.getProperty(OPTION_ANIMATEDURINGGAME, animateDuringGame);
        if (animateDuringGame && multiple_display && currentPage) {
            while (g_main_context_pending(nullptr)) {
                g_main_context_iteration(nullptr, false);
            }
            currentPage->update(delta); // Now using real delta from SDL
            for (int i = 1; i < SDL::getScreenCount(); ++i) {
                SDL_Renderer* renderer = SDL::getRenderer(i);
                SDL_Texture* target = SDL::getRenderTarget(i);
                if (renderer && target) {
                    SDL_SetRenderTarget(renderer, target);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                    SDL_RenderClear(renderer);
                    currentPage->draw(i);
                    SDL_SetRenderTarget(renderer, nullptr);
                    SDL_RenderCopy(renderer, target, nullptr, nullptr);
                    SDL_RenderPresent(renderer);
                }
            }
        }
        };



    if (isAttractMode) {
        int timeout = 30;
        config_.getProperty(OPTION_ATTRACTMODELAUNCHRUNTIME, timeout);
        auto attractModeInputCheck = [&inputMonitor]() { return inputMonitor.checkSdlEvents() != InputDetectionResult::NoInput; };
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
                // The restrictor is not engaged during attract mode demos. If the user
                // interrupts to play a 4-way game, we must engage the restrictor now.
                // The RAII guard will handle resetting it to 8-way upon function exit.
                if (restrictorEnabled && currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos) {
                    LOG_INFO("Launcher", "User taking over 4-way game in attract mode. Engaging restrictor.");
                    restrictorGuard.emplace(4);
                }
                CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
                int lastPlayedSize = 10;
                config_.getProperty(OPTION_LASTPLAYEDSIZE, lastPlayedSize);
                cib.updateLastPlayedPlaylist(currentPage->getCollection(), collectionItem, lastPlayedSize);

                auto quitCheck = [&inputMonitor]() { return inputMonitor.checkSdlEvents() == InputDetectionResult::QuitInput; };
                processManager->wait(0, quitCheck, onFrameTick);
                processManager->terminate();
            }
        }
        else if (result == WaitResult::Timeout) {
            LOG_INFO("Launcher", "Attract mode timeout reached. Terminating process.");
            processManager->terminate();
        }
    }
    else { // Normal mode
        LOG_INFO("Launcher", "Waiting for launched process to complete. Press quit combo to force quit.");
        auto quitCheck = [&inputMonitor]() { return inputMonitor.checkSdlEvents() == InputDetectionResult::QuitInput; };
        WaitResult result = processManager->wait(0, quitCheck, onFrameTick);

        if (result == WaitResult::UserInput) {
            LOG_INFO("Launcher", "User pressed quit combo during game. Terminating process.");
            processManager->terminate();
        }
        else {
            LOG_INFO("Launcher", "Process completed naturally.");
        }
    }

    endTime = std::chrono::steady_clock::now();

    // 6e. Update stats
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

    //
    // --- STEP 7: REBOOT CHECK ---
    //
    bool reboot = false;
    config_.getProperty("launchers." + launcherName + ".reboot", reboot);

    LOG_INFO("Launcher", "Execution completed for: " + executablePath + " with reboot flag: " + std::to_string(reboot));
    return reboot;
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

