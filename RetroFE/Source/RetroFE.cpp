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

#include "RetroFE.h"
#include "Collection/CollectionInfo.h"
#include "Collection/CollectionInfoBuilder.h"
#include "Collection/Item.h"
#include "Collection/MenuParser.h"
#include "Control/Restrictor/Restrictor.h"
#include "Control/Restrictor/RestrictorInstance.h"
#include "Control/UserInput.h"
#include "Database/Configuration.h"
#include "Database/GlobalOpts.h"
#include "Database/HiScores.h"
#include "Execute/Launcher.h"
#include "Graphics/Component/ScrollingList.h"
#include "Graphics/Page.h"
#include "Graphics/PageBuilder.h"
#include "Graphics/Component/Text.h"
#include "Sound/MusicPlayer.h"
#include "Menu/Menu.h"
#include "SDL.h"
#include "Utility/Log.h"
#include "Utility/Utils.h"
#include "Video/VideoFactory.h"
#include "Video/VideoPool.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gst/gst.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <cmath>
#if __has_include(<SDL2/SDL_ttf.h>)
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif

#if defined(__linux) || defined(__APPLE__)
#include <cstring>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#ifdef __linux
#include <libusb-1.0/libusb.h>
#endif

#ifdef WIN32
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_thread.h>
#include <Windows.h>
#include "StdAfx.h"
#endif

RetroFE::RetroFE(Configuration& c)
	: initialized(false), initializeError(false), initializeThread(NULL), config_(c), db_(NULL), metadb_(NULL),
	input_(config_), currentPage_(NULL), keyInputDisable_(0), currentTime_(0), lastLaunchReturnTime_(0),
	keyLastTime_(0), keyDelayTime_(.3f), reboot_(false), kioskLock_(false), paused_(false), buildInfo_(false),
	collectionInfo_(false), gameInfo_(false), musicPlayer_(nullptr) {
	menuMode_ = false;
	attractMode_ = false;
	attractModePlaylistCollectionNumber_ = 0;
	firstPlaylist_ = "all"; // todo
}

namespace fs = std::filesystem;

#ifdef __linux

bool InitializeServoStik() {
	libusb_context* ctx = NULL;
	int ret = libusb_init(&ctx);
	if (ret < 0) {
		fprintf(stderr, "libusb_init failed: %d\n", ret);
		return false;
	}

	// Hardcoded Ultimarc ServoStik vendor/product IDs
	libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, 0xD209, 0x1700);
	if (handle) {
		// Device found, close and return true
		libusb_close(handle);
		libusb_exit(ctx);
		return true;
	}

	// Device not found, exit libusb and return false
	libusb_exit(ctx);
	return false;
}
#endif

RetroFE::~RetroFE() {
	deInitialize();
}

void RetroFE::render() {
	static double accumulatedRenderMs = 0.0;
	static int framesSinceFpsUpdate = 0;
	static double displayedFps = 0.0;
	static double displayedRenderMs = 0.0;
	static uint64_t lastFpsUpdateTimestamp = 0;
	static bool prevShowFps = false; // Detect transition
	static bool waitingForFpsData = false;

	uint64_t r_startTicks = SDL_GetPerformanceCounter(); // 'r_' prefix for render-specific

	// --- 1. Clear render targets ---
	for (int i = 0; i < SDL::getScreenCount(); ++i) {
		SDL_Renderer* currentRenderer = SDL::getRenderer(i);
		SDL_Texture* currentRenderTarget = SDL::getRenderTarget(i);
		if (!currentRenderer || !currentRenderTarget) continue;

		SDL_SetRenderTarget(currentRenderer, currentRenderTarget);
		SDL_SetRenderDrawColor(currentRenderer, 0, 0, 0, 255);
		SDL_RenderClear(currentRenderer);
	}

	// --- 2. Draw main content and overlay ---
	if (currentPage_) {
		for (int i = 0; i < SDL::getScreenCount(); ++i) {
			SDL_Renderer* currentRenderer = SDL::getRenderer(i);
			SDL_Texture* currentRenderTarget = SDL::getRenderTarget(i);
			if (!currentRenderer || !currentRenderTarget) continue;

			SDL_SetRenderTarget(currentRenderer, currentRenderTarget);
			currentPage_->draw(i);

			if (showFps_ && i == 0 && fpsOverlayTexture_) {
				SDL_Rect dst = { 20, 20, fpsOverlayW_, fpsOverlayH_ };
				SDL_RenderCopy(currentRenderer, fpsOverlayTexture_, nullptr, &dst);
			}
		}
	}

	// --- 3. Present to screens ---
	for (int i = 0; i < SDL::getScreenCount(); ++i) {
		SDL_Renderer* currentRenderer = SDL::getRenderer(i);
		SDL_Texture* currentRenderTarget = SDL::getRenderTarget(i);
		if (!currentRenderer || !currentRenderTarget) continue;

		SDL_SetRenderTarget(currentRenderer, nullptr);
		SDL_RenderCopy(currentRenderer, currentRenderTarget, nullptr, nullptr);
		SDL_RenderPresent(currentRenderer); // Blocks if VSync is on
	}

	// --- 4. Timing for THIS render() call ---
	uint64_t r_endTicks = SDL_GetPerformanceCounter();
	uint64_t r_deltaTicks = r_endTicks - r_startTicks;
	double currentRenderDurationMs = (r_deltaTicks * 1000.0) / freq_;

	// --- 5. FPS display logic ---
	bool showFpsJustEnabled = (!prevShowFps && showFps_);
	prevShowFps = showFps_;

	if (showFpsJustEnabled) {
		lastFpsUpdateTimestamp = SDL_GetTicks64();
		framesSinceFpsUpdate = 0;
		accumulatedRenderMs = 0.0;
		waitingForFpsData = true;
	}

	if (showFps_) {
		framesSinceFpsUpdate++;
		accumulatedRenderMs += currentRenderDurationMs;

		uint64_t now_ticks64 = SDL_GetTicks64();
		if (now_ticks64 - lastFpsUpdateTimestamp >= 1000) {
			displayedFps = framesSinceFpsUpdate * 1000.0 / (now_ticks64 - lastFpsUpdateTimestamp);
			displayedRenderMs = accumulatedRenderMs / std::max(1, framesSinceFpsUpdate);

			lastFpsUpdateTimestamp = now_ticks64 - (now_ticks64 - lastFpsUpdateTimestamp) % 1000;
			framesSinceFpsUpdate = 0;
			accumulatedRenderMs = 0.0;
			waitingForFpsData = false;  // Got real data now
		}

		// --- Compose overlay text ---
		char overlayText[128];
		if (waitingForFpsData) {
			snprintf(overlayText, sizeof(overlayText), "FPS: -- | Frame: -- ms | Draw: -- ms");
		}
		else {
			snprintf(overlayText, sizeof(overlayText), "FPS: %.1f | Frame: %.2f ms | Draw: %.2f ms",
				displayedFps, this->lastFrameTimeMs_, displayedRenderMs);
		}

		if (lastOverlayText_ != overlayText) {
			lastOverlayText_ = overlayText;

			if (fpsOverlayTexture_) {
				SDL_DestroyTexture(fpsOverlayTexture_);
				fpsOverlayTexture_ = nullptr;
			}

			if (debugFont_) {
				SDL_Color color = { 255, 255, 0, 255 };
				SDL_Surface* surf = TTF_RenderText_Blended(debugFont_, overlayText, color);
				if (surf) {
					SDL_Renderer* renderer0 = SDL::getRenderer(0);
					if (renderer0) {
						fpsOverlayTexture_ = SDL_CreateTextureFromSurface(renderer0, surf);
						fpsOverlayW_ = surf->w;
						fpsOverlayH_ = surf->h;
					}
					SDL_FreeSurface(surf);
				}
			}
		}
	}

	else {
		if (fpsOverlayTexture_) {
			SDL_DestroyTexture(fpsOverlayTexture_);
			fpsOverlayTexture_ = nullptr;
			fpsOverlayW_ = 0;
			fpsOverlayH_ = 0;
			lastOverlayText_ = "";
		}
		accumulatedRenderMs = 0.0;
		framesSinceFpsUpdate = 0;
		waitingForFpsData = false;
	}
}

// Initialize the configuration and database
int RetroFE::initialize(void* context) {

	auto* instance = static_cast<RetroFE*>(context);

	LOG_INFO("RetroFE", "Initializing");

	if (!instance->input_.initialize())
	{
		LOG_ERROR("RetroFE", "Could not initialize user controls");
		instance->initializeError = true;
		return -1;
	}
	instance->db_ = new DB(Utils::combinePath(Configuration::absolutePath, "meta.db"));

	if (!instance->db_->initialize())
	{
		LOG_ERROR("RetroFE", "Could not initialize database");
		instance->initializeError = true;
		return -1;
	}
	instance->metadb_ = new MetadataDatabase(*(instance->db_), instance->config_);

	if (!instance->metadb_->initialize())
	{
		LOG_ERROR("RetroFE", "Could not initialize meta database");
		instance->initializeError = true;
		return -1;
	}

	instance->initializeMusicPlayer();


	// Initialize HiScores
	std::string zipPath = Utils::combinePath(Configuration::absolutePath, "hi2txt", "hi2txt_defaults.zip");
	std::string overridePath = Utils::combinePath(Configuration::absolutePath, "hi2txt", "scores");

	HiScores::getInstance().loadHighScores(zipPath, overridePath);

	instance->initialized = true;
	return 0;
}

void RetroFE::initializeMusicPlayer() {
	// Initialize music player
	bool musicPlayerEnabled = false;
	config_.getProperty("musicPlayer.enabled", musicPlayerEnabled);
	if (musicPlayerEnabled)
	{
		if (Mix_Init(MIX_INIT_MP3) != 8) {
			LOG_ERROR("MusicPlayer", "Failed to initialize SDL_mixer for MP3 support");
		}
		else
		{
			LOG_INFO("MusicPlayer", "SDL_mixer initialized for MP3 support");
		}
		musicPlayer_ = MusicPlayer::getInstance();
		if (!musicPlayer_->initialize(config_))
		{
			LOG_ERROR("RetroFE", "Failed to initialize music player");
		}
		else
		{
			LOG_INFO("RetroFE", "Music player initialized successfully");
		}
	}
	else {
		LOG_INFO("RetroFE", "Music player disabled by configuration");
	}
}

// Launch a game/program
void RetroFE::launchEnter() {
	currentPage_->setIsLaunched(true);
	// Disable window focus
	SDL_SetWindowGrab(SDL::getWindow(0), SDL_FALSE);
	// Free textures and shut down SDL if unloadSDL flag is set
	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
	if (unloadSDL)
	{
		freeGraphicsMemory();
	}
#ifdef __APPLE__
	SDL_SetRelativeMouseMode(SDL_FALSE);
#endif

	if (musicPlayer_) {
		bool musicPlayerPlayInGame = false;
		config_.getProperty("musicPlayer.playInGame", musicPlayerPlayInGame);
		if (musicPlayerPlayInGame)
		{
			int musicPlayerPlayInGameVol = -1;
			if (config_.getProperty("musicPlayer.playInGameVol", musicPlayerPlayInGameVol))
			{
				// Only proceed if the value is in the valid range (0�100).
				if (musicPlayerPlayInGameVol >= 0 && musicPlayerPlayInGameVol <= 100)
				{
					// Get current volume (0�128) and convert to percentage (0�100)
					int currentVolume = musicPlayer_->getVolume();
					int currentVolumePercent = static_cast<int>((currentVolume / static_cast<float>(MIX_MAX_VOLUME)) * 100.0f + 0.5f);

					// Only perform the fade if the current volume is greater than or equal to the target.
					if (currentVolumePercent >= musicPlayerPlayInGameVol) {
						musicPlayer_->fadeToVolume(musicPlayerPlayInGameVol);
					}
					// Otherwise, no fade is performed.
				}
			}
		}
		else
		{
			musicPlayer_->pauseMusic();
		}
	}
#ifdef WIN32
	Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 75, 0);
#endif
}

// Return from the launch of a game/program
void RetroFE::launchExit() {
	currentPage_->setIsLaunched(false);
	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
	if (unloadSDL)
	{
		allocateGraphicsMemory();
	}

	SDL_RestoreWindow(SDL::getWindow(0));
	SDL_RaiseWindow(SDL::getWindow(0));
	SDL_SetWindowGrab(SDL::getWindow(0), SDL_TRUE);

	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		if (e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED)
		{
			input_.update(e);
		}
	}
	input_.resetStates();
	currentPage_->updateReloadables(0);
	currentPage_->onNewItemSelected();
	currentPage_->reallocateMenuSpritePoints(false);
	attract_.reset(false);
	currentTime_ = static_cast<float>((SDL_GetPerformanceCounter() * 1.0 / freq_)); // currentTime_ in seconds
	keyLastTime_ = currentTime_;
	lastLaunchReturnTime_ = currentTime_;

#ifndef __APPLE__
	SDL_WarpMouseInWindow(SDL::getWindow(0), SDL::getWindowWidth(0), 0);
#endif

	bool musicPlayerPlayInGame = false;
	config_.getProperty("musicPlayer.playInGame", musicPlayerPlayInGame);
	if (musicPlayer_ && musicPlayerPlayInGame)
	{
		int musicPlayerPlayInGameVol = -1;
		if (config_.getProperty("musicPlayer.playInGameVol", musicPlayerPlayInGameVol))
		{
			if (musicPlayerPlayInGameVol >= 0 && musicPlayerPlayInGameVol <= 100)
			{
				// Convert the target volume from percentage to MIX's range (0�128)
				int targetMixVolume = static_cast<int>((musicPlayerPlayInGameVol / 100.0f) * MIX_MAX_VOLUME + 0.5f);
				// Allow for a small rounding tolerance
				if (std::abs(musicPlayer_->getVolume() - targetMixVolume) <= 1)
				{
					// Restore the previous volume that was stored when fadeToVolume was called.
					musicPlayer_->fadeBackToPreviousVolume();
				}
				// Otherwise, do nothing (i.e. no fade-back is needed).
			}
		}
	}
	else if (musicPlayer_ && !musicPlayerPlayInGame)
	{
		musicPlayer_->resumeMusic();
	}

#ifdef WIN32
	Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 76, 0);
#endif
#ifdef __APPLE__
	SDL_SetRelativeMouseMode(SDL_TRUE);
#endif
}
// Free the textures, and optionall take down SDL
void RetroFE::freeGraphicsMemory() {
	// Free textures
	if (currentPage_)
	{
		currentPage_->freeGraphicsMemory();
	}

	// Close down SDL
	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
	if (unloadSDL)
	{
		// Ensure currentPage_ is not null before calling deInitializeFonts
		if (currentPage_)
		{
			currentPage_->deInitializeFonts();
		}

		SDL::deInitialize();
		input_.clearJoysticks();
	}
}

// Optionally set up SDL, and load the textures
void RetroFE::allocateGraphicsMemory() {

	// Reopen SDL
	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
	if (unloadSDL)
	{
		SDL::initialize(config_);
		currentPage_->initializeFonts();
	}

	// Allocate textures
	if (currentPage_)
	{
		currentPage_->allocateGraphicsMemory();
	}

}

// Deinitialize RetroFE
bool RetroFE::deInitialize() {

	bool retVal = true;

	// Free textures
	freeGraphicsMemory();

	VideoPool::shutdown();

	// Delete page
	if (currentPage_)
	{
		currentPage_->deInitialize();
		delete currentPage_;
		currentPage_ = nullptr;
	}

	// Delete databases
	if (metadb_)
	{
		delete metadb_;
		metadb_ = nullptr;
	}

	if (db_)
	{
		delete db_;
		db_ = nullptr;
	}

	if (musicPlayer_)
	{
		musicPlayer_->shutdown();
	}

	if (debugFont_) {
		TTF_CloseFont(debugFont_);
		debugFont_ = nullptr;
	}

	if (fpsOverlayTexture_)
	{
		SDL_DestroyTexture(fpsOverlayTexture_);
		fpsOverlayTexture_ = nullptr;
	}

	initialized = false;

	if (reboot_)
	{
		LOG_INFO("RetroFE", "Rebooting");
	}
	else
	{
		LOG_INFO("RetroFE", "Exiting");
		gst_deinit();
		SDL::deInitialize();
	}

	return retVal;
}

// Run RetroFE
bool RetroFE::run() {
	std::string controlsConfPath = Utils::combinePath(Configuration::absolutePath, "controls");
	if (!fs::exists(controlsConfPath + ".conf"))
	{
		std::string logFile = Utils::combinePath(Configuration::absolutePath, "log.txt");
		if (Utils::isOutputATerminal())
		{
			fprintf(stderr,
				"RetroFE failed to find a valid controls.conf in the current directory\nCheck the log for details: "
				"%s\n",
				logFile.c_str());
		}
		else
		{
			SDL_ShowSimpleMessageBox(
				SDL_MESSAGEBOX_ERROR, "Configuration Error",
				("RetroFE failed to find a valid controls.conf in the current directory\nCheck the log for details: " +
					logFile)
				.c_str(),
				NULL);
		}
		exit(EXIT_FAILURE);
	}

	// Initialize SDL
	if (!SDL::initialize(config_))
		return false;
	if (!fontcache_.initialize())
		return false;

	config_.getProperty(OPTION_SHOWFPS, showFps_);
	if (showFps_)
	{
		std::string fontPath = Configuration::absolutePath + "/font.ttf";
		debugFont_ = TTF_OpenFont(fontPath.c_str(), 24);
		if (!debugFont_)
		{
			LOG_ERROR("RetroFE", "Could not load font: " + fontPath);
			return false;
		}
		else
		{
			LOG_INFO("RetroFE", "Loaded font: " + fontPath);
		}
	}
	else
	{
		debugFont_ = nullptr;
	}

	SDL_RestoreWindow(SDL::getWindow(0));
	SDL_RaiseWindow(SDL::getWindow(0));
	SDL_SetWindowGrab(SDL::getWindow(0), SDL_TRUE);

	gRestrictor = IRestrictor::create();
	config_.setProperty("restrictorEnabled", gRestrictor != nullptr);

	// Define control configuration
	config_.import("controls", controlsConfPath + ".conf");
	for (int i = 1; i < 10; i++)
	{
		std::string numberedControlsFile = controlsConfPath + std::to_string(i) + ".conf";
		if (fs::exists(numberedControlsFile))
		{
			config_.import("controls", numberedControlsFile, false);
		}
	}

	if (config_.propertiesEmpty())
	{
		LOG_ERROR("RetroFE", "No controls.conf found");
		return false;
	}

	float preloadTime = 0;

	// Initialize video
	bool videoEnable = true;
	int videoLoop = 0;
	config_.getProperty(OPTION_VIDEOENABLE, videoEnable);
	config_.getProperty(OPTION_VIDEOLOOP, videoLoop);
	VideoFactory::setEnabled(videoEnable);
	VideoFactory::setNumLoops(videoLoop);

	initializeThread = SDL_CreateThread(initialize, "RetroFEInit", (void*)this);

	if (!initializeThread)
	{
		LOG_INFO("RetroFE", "Could not initialize RetroFE");
		return false;
	}

	bool attractModeFast = false;
	int attractModeTime = 0;
	int attractModeNextTime = 0;
	int attractModePlaylistTime = 0;
	int attractModeCollectionTime = 0;
	int attractModeMinTime = 1000;
	int attractModeMaxTime = 5000;
	bool attractModeLaunch = false;
	std::string attractModeLaunchMinMaxScrolls = "3,5";

	std::string firstCollection = "Main";
	bool running = true;
	state_ = RETROFE_NEW;

	config_.getProperty(OPTION_ATTRACTMODETIME, attractModeTime);
	config_.getProperty(OPTION_ATTRACTMODENEXTTIME, attractModeNextTime);
	config_.getProperty(OPTION_ATTRACTMODEPLAYLISTTIME, attractModePlaylistTime);
	config_.getProperty(OPTION_ATTRACTMODECOLLECTIONTIME, attractModeCollectionTime);
	config_.getProperty(OPTION_ATTRACTMODEMINTIME, attractModeMinTime);
	config_.getProperty(OPTION_ATTRACTMODEMAXTIME, attractModeMaxTime);
	config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
	config_.getProperty(OPTION_ATTRACTMODEFAST, attractModeFast);
	config_.getProperty(OPTION_ATTRACTMODELAUNCH, attractModeLaunch);
	config_.getProperty(OPTION_ATTRACTMODELAUNCHMINMAXSCROLLS, attractModeLaunchMinMaxScrolls);
	std::vector<std::string> attMinMaxVec;
	Utils::listToVector(attractModeLaunchMinMaxScrolls, attMinMaxVec, ',');

	attract_.idleTime = static_cast<float>(attractModeTime);
	attract_.idleNextTime = static_cast<float>(attractModeNextTime);
	attract_.idlePlaylistTime = static_cast<float>(attractModePlaylistTime);
	attract_.idleCollectionTime = static_cast<float>(attractModeCollectionTime);
	attract_.minTime = attractModeMinTime;
	attract_.maxTime = attractModeMaxTime;
	attract_.isFast = attractModeFast;
	attract_.shouldLaunch = attractModeLaunch;
	attract_.setLaunchFrequencyRange(Utils::convertInt(attMinMaxVec[0]), Utils::convertInt(attMinMaxVec[1]));

	int fps = 60;
	int fpsIdle = 60;
	config_.getProperty(OPTION_FPS, fps);
	config_.getProperty(OPTION_FPSIDLE, fpsIdle);
	double fpsTime = 1000.0 / static_cast<double>(fps);
	double fpsIdleTime = 1000.0 / static_cast<double>(fpsIdle);
	bool vSync = false;
	config_.getProperty(OPTION_VSYNC, vSync);

	int initializeStatus = 0;
	bool inputClear = false;

	// load the initial splash screen, unload it once it is complete
	currentPage_ = loadSplashPage();
	state_ = RETROFE_ENTER;
	bool splashMode = true;
	bool exitSplashMode = false;
	// don't show splash
	bool screensaver = false;
	config_.getProperty(OPTION_SCREENSAVER, screensaver);

	Launcher l(config_, *this);
	Menu m(config_, input_);
	preloadTime = static_cast<float>(SDL_GetTicks()) / 1000;

	l.LEDBlinky(1);
	l.startScript();
	config_.getProperty(OPTION_KIOSK, kioskLock_);

	// settings button
	std::string settingsCollection = "";
	std::string settingsPlaylist = "settings";
	std::string settingsCollectionPlaylist;
	config_.getProperty(OPTION_SETTINGSCOLLECTIONPLAYLIST, settingsCollectionPlaylist);
	if (size_t position = settingsCollectionPlaylist.find(":"); position != std::string::npos)
	{
		settingsCollection = settingsCollectionPlaylist.substr(0, position);
		settingsPlaylist = settingsCollectionPlaylist.erase(0, position + 1);
		config_.setProperty("settingsPlaylist", settingsPlaylist);
	}

	// quickList button
	std::string quickListCollection = "";
	std::string quickListPlaylist = "quicklist";
	std::string quickListCollectionPlaylist;
	config_.getProperty(OPTION_QUICKLISTCOLLECTIONPLAYLIST, quickListCollectionPlaylist);
	if (size_t position = quickListCollectionPlaylist.find(":"); position != std::string::npos)
	{
		quickListCollection = quickListCollectionPlaylist.substr(0, position);
		quickListPlaylist = quickListCollectionPlaylist.erase(0, position + 1);
		config_.setProperty("quickListPlaylist", quickListPlaylist);
	}

	float deltaTime = 0;
	float inputUpdateInterval = 0.0333f; // Update every ~33.33ms (~30Hz)
	static float lastInputUpdateTime = 0.0f;

	double initial_current_time_ms = SDL_GetPerformanceCounter() * 1000.0 / (double)freq_;

	double nextFrameTime = initial_current_time_ms;
	lastFrameTimePointMs_ = initial_current_time_ms;

	float glibUpdateInterval = 0.016f; // ~16ms (60Hz)
	static float glibAccumulator = 0.0f;

	while (running)
	{
		uint64_t loopStart = SDL_GetPerformanceCounter();
		double nowMs_loopStart = loopStart * 1000.0 / freq_; // freq is SDL_GetPerformanceFrequency()

		if (nextFrameTime < nowMs_loopStart) {
			nextFrameTime = nowMs_loopStart;
		}

		deltaTime = static_cast<float>((nowMs_loopStart - lastFrameTimePointMs_) / 1000.0);
		
		// If dt is over 100ms (0.1s), clamp it to a "sane" frame time (e.g. 1/60th = 0.0167)
		// This avoids animation jumps after game launches or big stalls
		if (deltaTime > 0.1f) deltaTime = 0.0167f;
		
		currentTime_ = static_cast<float>(nowMs_loopStart / 1000.0);
		lastFrameTimePointMs_ = nowMs_loopStart; // For next frame's deltaTime

		// ----- GLib processing -----
		glibAccumulator += deltaTime;
		while (glibAccumulator >= glibUpdateInterval) {
			while (g_main_context_pending(nullptr)) {
				g_main_context_iteration(nullptr, false);
			}
			glibAccumulator -= glibUpdateInterval;
		}

		// Exit splash mode when an active key is pressed
		if (SDL_Event e; splashMode && (SDL_PollEvent(&e)))
		{
			if (screensaver || input_.update(e))
			{
				if (screensaver || input_.keystate(UserInput::KeyCodeSelect))
				{
					exitSplashMode = true;
					while (SDL_PollEvent(&e))
					{
						if (e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED)
						{
							input_.update(e);
						}
					}
					input_.resetStates();
					attract_.reset();
				}
				else if (input_.keystate(UserInput::KeyCodeQuit))
				{
					l.exitScript();
					running = false;
					break;
				}
			}
		}

		if (!currentPage_)
		{
			LOG_WARNING("RetroFE", "Could not load page");
			l.exitScript();
			running = false;
			break;
		}

		switch (state_)
		{

			// Idle state; waiting for input
			case RETROFE_IDLE:

			currentPage_->cleanup();

			// Not in splash mode
			if (currentPage_ && !splashMode)
			{
				// account for when returning from a menu and the previous key was still "stuck"
				if (lastLaunchReturnTime_ == 0 || (currentTime_ - lastLaunchReturnTime_ > .3))
				{
					if (currentPage_->isIdle())
					{
						state_ = processUserInput(currentPage_);
					}
					lastLaunchReturnTime_ = 0;
				}
			}

			// Handle end of splash mode
			if ((initialized || initializeError) && splashMode &&
				(exitSplashMode ||
					(currentPage_->getMinShowTime() <= (currentTime_ - preloadTime) && !(currentPage_->isPlaying()))))
			{
				SDL_WaitThread(initializeThread, &initializeStatus);

				if (initializeError)
				{
					setState(RETROFE_QUIT_REQUEST);
					break;
				}
				currentPage_->stop();
				setState(RETROFE_SPLASH_EXIT);
			}

			break;

			// Load art on entering RetroFE
			case RETROFE_LOAD_ART:
			currentPage_->start();
#ifdef WIN32
			Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 50, 0);
#endif
			setState(RETROFE_ENTER);
			break;

			// Wait for onEnter animation to finish
			case RETROFE_ENTER:
			if (currentPage_->isIdle())
			{
				bool startCollectionEnter = false;
				config_.getProperty(OPTION_STARTCOLLECTIONENTER, startCollectionEnter);
				nextPageItem_ = currentPage_->getSelectedItem();
				if (!splashMode && startCollectionEnter && !nextPageItem_->leaf)
				{
					setState(RETROFE_NEXT_PAGE_REQUEST);
				}
				else
				{
					setState(RETROFE_IDLE);
				}
			}
			break;

			// Handle end of splash mode
			case RETROFE_SPLASH_EXIT:
			if (currentPage_->isIdle()) {
				// Save layout information before cleanup
				int currentLayout = currentPage_->getCurrentLayout();

				// Properly cleanup splash page
				currentPage_->stop();
				currentPage_->deInitialize();
				delete currentPage_;
				currentPage_ = nullptr;  // Prevent dangling pointer

				// Find first collection to load
				std::string firstCollection = "Main";
				config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);

				// Load new page with error checking
				currentPage_ = loadPage(firstCollection);
				if (!currentPage_) {
					LOG_ERROR("RetroFE", "Failed to load initial page after splash");
					setState(RETROFE_QUIT_REQUEST);
					break;
				}

				// Restore layout settings and setup initial state        
				currentPage_->setCurrentLayout(currentLayout);
				currentPage_->setLocked(kioskLock_);

				// Add collections to cycle
				std::string cycleString;
				config_.getProperty(OPTION_CYCLECOLLECTION, cycleString);
				Utils::listToVector(cycleString, collectionCycle_, ',');
				collectionCycleIt_ = collectionCycle_.begin();

				// Update current collection configuration
				cycleVector_.clear();
				config_.setProperty("currentCollection", firstCollection);

				// Get collection info with error checking
				CollectionInfo* info = getCollection(firstCollection);
				if (!info) {
					LOG_ERROR("RetroFE", "Failed to load collection info for: " + firstCollection);
					setState(RETROFE_QUIT_REQUEST);
					break;
				}

				// Push collection to page
				if (!currentPage_->pushCollection(info)) {
					LOG_ERROR("RetroFE", "Failed to push collection to page: " + firstCollection);
					delete info;  // Clean up on failure
					setState(RETROFE_QUIT_REQUEST);
					break;
				}

				// Get first playlist setting
				config_.getProperty(OPTION_FIRSTPLAYLIST, firstPlaylist_);

				// Check collection-specific playlist setting if needed
				if (firstPlaylist_.empty() || firstCollection != currentPage_->getCollectionName()) {
					std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					if (config_.propertyExists(settingPrefix + OPTION_FIRSTPLAYLIST)) {
						config_.getProperty(settingPrefix + OPTION_FIRSTPLAYLIST, firstPlaylist_);
					}
				}

				// Handle favorites collection special case
				if (currentPage_->getCollectionName() == "Favorites") {
					firstPlaylist_ = "favorites";
				}

				// Set initial playlist with fallback
				currentPage_->selectPlaylist(firstPlaylist_);
				if (currentPage_->getPlaylistName() != firstPlaylist_) {
					currentPage_->selectPlaylist("all");
				}

				// Handle screensaver/random start settings
				bool randomStart = false;
				bool screensaver = false;
				config_.getProperty(OPTION_RANDOMSTART, randomStart);
				config_.getProperty(OPTION_SCREENSAVER, screensaver);

				if (screensaver || randomStart) {
					if (currentPage_->getPlaylistName() == "all")
						currentPage_->selectRandomPlaylist(info, getPlaylistCycle());
					currentPage_->selectRandom();
				}

				// Initialize display
				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints();  // Update playlist menu

				splashMode = false;

				if (musicPlayer_) {
					// Check if music should auto-start
					bool autoStart = false;
					if (config_.getProperty("musicPlayer.autostart", autoStart) && autoStart)
					{
						LOG_INFO("RetroFE", "Auto-starting music player");
						bool shuffle = true;
						config_.getProperty("musicPlayer.shuffle", shuffle);

						if (shuffle)
						{
							musicPlayer_->shuffle();
						}
						else
						{
							musicPlayer_->playMusic(0); // Start with first track
						}
					}
				}

				setState(RETROFE_LOAD_ART);
			}
			break;

			case RETROFE_GAMEINFO_ENTER:
			currentPage_->gameInfoEnter();
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_GAMEINFO_EXIT:
			currentPage_->gameInfoExit();
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_COLLECTIONINFO_ENTER:
			currentPage_->collectionInfoEnter();
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_COLLECIONINFO_EXIT:
			currentPage_->collectionInfoExit();
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_BUILDINFO_ENTER:
			currentPage_->buildInfoEnter();
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_BUILDINFO_EXIT:
			currentPage_->buildInfoExit();
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_PLAYLIST_NEXT:
			currentPage_->nextPlaylist();
			setState(RETROFE_PLAYLIST_REQUEST);
			break;

			case RETROFE_PLAYLIST_PREV:
			currentPage_->playlistPrevEnter();
			currentPage_->prevPlaylist();
			setState(RETROFE_PLAYLIST_REQUEST);
			break;
			case RETROFE_SCROLL_FORWARD:
			if (currentPage_->isIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionForward);
				currentPage_->scroll(true, false);
				currentPage_->updateScrollPeriod();
			}
			setState(RETROFE_IDLE);
			break;
			case RETROFE_SCROLL_BACK:
			if (currentPage_->isIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionBack);
				currentPage_->scroll(false, false);
				currentPage_->updateScrollPeriod();
			}
			setState(RETROFE_IDLE);
			break;
			case RETROFE_SCROLL_PLAYLIST_FORWARD:
			if (currentPage_->isIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionPlaylistForward);
				currentPage_->scroll(true, true);
				currentPage_->updateScrollPeriod();
			}
			setState(RETROFE_IDLE);
			break;
			case RETROFE_SCROLL_PLAYLIST_BACK:
			if (currentPage_->isIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionPlaylistBack);
				currentPage_->scroll(false, true);
				currentPage_->updateScrollPeriod();
			}
			setState(RETROFE_IDLE);
			break;
			case RETROFE_QUICKLIST_REQUEST:
			currentPage_->playlistExit();
			currentPage_->resetScrollPeriod();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			setState(RETROFE_QUICKLIST_PAGE_MENU_EXIT);
			break;
			case RETROFE_QUICKLIST_PAGE_MENU_EXIT:
			if ((quickListCollection == "" || currentPage_->getCollectionName() == quickListCollection) &&
				(quickListPlaylist == "" || currentPage_->getPlaylistName() == quickListPlaylist))
			{
				if (nextPageItem_) {
					delete nextPageItem_;
				}
				nextPageItem_ = new Item();
				config_.getProperty("lastCollection", nextPageItem_->name);
				if (currentPage_->getCollectionName() != nextPageItem_->name)
				{
					setState(RETROFE_BACK_MENU_EXIT);
				}
				else
				{
					setState(RETROFE_PLAYLIST_REQUEST);
					// return to last playlist
					// todo move to function for re-use
					bool rememberMenu = false;
					config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);

					std::string autoPlaylist = "all";

					if (std::string quickListPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(quickListPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(quickListPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}

					if (currentPage_->getCollectionName() == "Favorites")
					{
						autoPlaylist = "favorites";
					}

					bool returnToRememberedPlaylist =
						rememberMenu && lastMenuPlaylists_.find(nextPageItem_->name) != lastMenuPlaylists_.end();
					if (returnToRememberedPlaylist)
					{
						currentPage_->selectPlaylist(
							lastMenuPlaylists_[nextPageItem_->name]); // Switch to last playlist
					}
					else
					{
						currentPage_->selectPlaylist(autoPlaylist);
						if (currentPage_->getPlaylistName() != autoPlaylist)
							currentPage_->selectPlaylist("all");
					}

					if (returnToRememberedPlaylist)
					{
						if (lastMenuOffsets_.size() &&
							lastMenuPlaylists_.find(nextPageItem_->name) != lastMenuPlaylists_.end())
						{
							currentPage_->setScrollOffsetIndex(lastMenuOffsets_[nextPageItem_->name]);
						}
					}
				}
				break;
			}
			resetInfoToggle();
			setState(RETROFE_QUICKLIST_PAGE_REQUEST);
			break;
			case RETROFE_SETTINGS_REQUEST:
			currentPage_->playlistExit();
			currentPage_->resetScrollPeriod();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			setState(RETROFE_SETTINGS_PAGE_MENU_EXIT);
			break;
			case RETROFE_SETTINGS_PAGE_MENU_EXIT:
			if ((settingsCollection == "" || currentPage_->getCollectionName() == settingsCollection) &&
				(settingsPlaylist == "" || currentPage_->getPlaylistName() == settingsPlaylist))
			{
				nextPageItem_ = new Item();
				config_.getProperty("lastCollection", nextPageItem_->name);
				if (currentPage_->getCollectionName() != nextPageItem_->name)
				{
					setState(RETROFE_BACK_MENU_EXIT);
				}
				else
				{
					setState(RETROFE_PLAYLIST_REQUEST);
					// return to last playlist
					// todo move to function for re-use
					bool rememberMenu = false;
					config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);

					std::string autoPlaylist = "all";

					if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}

					if (currentPage_->getCollectionName() == "Favorites")
					{
						autoPlaylist = "favorites";
					}

					bool returnToRememberedPlaylist =
						rememberMenu && lastMenuPlaylists_.find(nextPageItem_->name) != lastMenuPlaylists_.end();
					if (returnToRememberedPlaylist)
					{
						currentPage_->selectPlaylist(
							lastMenuPlaylists_[nextPageItem_->name]); // Switch to last playlist
					}
					else
					{
						currentPage_->selectPlaylist(autoPlaylist);
						if (currentPage_->getPlaylistName() != autoPlaylist)
							currentPage_->selectPlaylist("all");
					}

					if (returnToRememberedPlaylist)
					{
						if (lastMenuOffsets_.size() &&
							lastMenuPlaylists_.find(nextPageItem_->name) != lastMenuPlaylists_.end())
						{
							currentPage_->setScrollOffsetIndex(lastMenuOffsets_[nextPageItem_->name]);
						}
					}
				}
				break;
			}
			resetInfoToggle();
			setState(RETROFE_SETTINGS_PAGE_REQUEST);
			break;
			case RETROFE_PLAYLIST_PREV_CYCLE:
			currentPage_->playlistPrevEnter();
			currentPage_->prevCyclePlaylist(getPlaylistCycle());
			// random highlight on first playlist cycle
			//selectRandomOnFirstCycle();

			setState(RETROFE_PLAYLIST_REQUEST);
			break;
			case RETROFE_PLAYLIST_NEXT_CYCLE:
			currentPage_->nextCyclePlaylist(getPlaylistCycle());
			// random highlight on first playlist cycle
			//selectRandomOnFirstCycle();

			setState(RETROFE_PLAYLIST_REQUEST);
			break;

			// Switch playlist; start onHighlightExit animation
			case RETROFE_PLAYLIST_REQUEST:

			inputClear = false;
			config_.getProperty(OPTION_PLAYLISTINPUTCLEAR, inputClear);
			if (inputClear)
			{
				// Empty event queue
				SDL_Event e;
				while (SDL_PollEvent(&e))
					input_.update(e);
				input_.resetStates();
			}
			currentPage_->playlistExit();
			currentPage_->resetScrollPeriod();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);

			setState(RETROFE_PLAYLIST_EXIT);
			break;

			// Switch playlist; wait for onHighlightExit animation to finish; load art
			case RETROFE_PLAYLIST_EXIT:
			if (currentPage_->isIdle())
			{
				// lots of different toggles and menu jumps trigger this by accident
				if (currentPage_->fromPlaylistNav)
				{
					if (currentPage_->fromPreviousPlaylist)
					{
						currentPage_->playlistPrevExit();
					}
					else
					{
						currentPage_->playlistNextExit();
					}
				}

				setState(RETROFE_PLAYLIST_LOAD_ART);
			}
			break;

			// Switch playlist; start onHighlightEnter animation
			case RETROFE_PLAYLIST_LOAD_ART:
			if (currentPage_->isIdle())
			{



				currentPage_->onNewItemSelected();

				currentPage_->reallocateMenuSpritePoints(); // update playlist menu
				currentPage_->playlistEnter();
				setState(RETROFE_PLAYLIST_ENTER);
			}
			break;

			// Switch playlist; wait for onHighlightEnter animation to finish
			case RETROFE_PLAYLIST_ENTER:
			if (currentPage_->isIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;

			// Jump in menu; start onMenuJumpExit animation
			case RETROFE_MENUJUMP_REQUEST:
			inputClear = false;
			config_.getProperty(OPTION_JUMPINPUTCLEAR, inputClear);
			if (inputClear)
			{
				// Empty event queue
				SDL_Event e;
				while (SDL_PollEvent(&e))
					input_.update(e);
				input_.resetStates();
			}
			currentPage_->menuJumpExit();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			setState(RETROFE_MENUJUMP_EXIT);
			break;

			// Jump in menu; wait for onMenuJumpExit animation to finish; load art
			case RETROFE_MENUJUMP_EXIT:
			if (currentPage_->isIdle())
			{
				setState(RETROFE_MENUJUMP_LOAD_ART);
			}
			break;

			// Jump in menu; start onMenuJumpEnter animation
			case RETROFE_MENUJUMP_LOAD_ART:
			if (currentPage_->isIdle())
			{
				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints(false); // skip updating playlist menu
				currentPage_->menuJumpEnter();
				setState(RETROFE_MENUJUMP_ENTER);
			}
			break;

			// Jump in menu; wait for onMenuJump animation to finish
			case RETROFE_MENUJUMP_ENTER:
			if (currentPage_->isIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;

			// Start onHighlightExit animation
			case RETROFE_HIGHLIGHT_REQUEST:
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			currentPage_->highlightExit();
			setState(RETROFE_HIGHLIGHT_EXIT);
			break;

			// Wait for onHighlightExit animation to finish; load art
			case RETROFE_HIGHLIGHT_EXIT:
			if (currentPage_->isIdle())
			{
				currentPage_->highlightLoadArt();
				setState(RETROFE_HIGHLIGHT_LOAD_ART);
			}
			break;

			// Start onHighlightEnter animation
			case RETROFE_HIGHLIGHT_LOAD_ART:
			currentPage_->highlightEnter();
			if (currentPage_->getSelectedItem())
				l.LEDBlinky(9, currentPage_->getSelectedItem()->collectionInfo->name, currentPage_->getSelectedItem());
			setState(RETROFE_HIGHLIGHT_ENTER);
			break;

			// Wait for onHighlightEnter animation to finish
			case RETROFE_HIGHLIGHT_ENTER:
			// detect that playlist selected is different the current then go to that playlist
			if (currentPage_->isMenuIdle() && currentPage_->getPlaylistMenu())
			{
				std::string selected_playlist = currentPage_->getPlaylistMenu()->getSelectedItem()->name;
				if (selected_playlist != currentPage_->getPlaylistName())
				{
					currentPage_->selectPlaylist(selected_playlist);
					setState(RETROFE_PLAYLIST_EXIT);
					break;
				}
			}
			if (RETROFE_STATE state_tmp = processUserInput(currentPage_);
				currentPage_->isMenuIdle() &&
				(state_tmp == RETROFE_HIGHLIGHT_REQUEST || state_tmp == RETROFE_MENUJUMP_REQUEST ||
					state_tmp == RETROFE_PLAYLIST_REQUEST))
			{
				state_ = state_tmp;
			}
			else if (currentPage_->isIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;
			case RETROFE_QUICKLIST_PAGE_REQUEST:
			if (currentPage_->isIdle() && currentPage_->getCollectionName() != "")
			{
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				if (currentPage_->getPlaylistName() != settingsCollectionPlaylist)
				{
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				}
				config_.setProperty("lastCollection", collectionName);

				setState(RETROFE_PLAYLIST_REQUEST);
				if (quickListCollection != "" && quickListCollection != collectionName)
				{
					setState(RETROFE_NEXT_PAGE_MENU_LOAD_ART);

					// update new current collection
					cycleVector_.clear();
					config_.setProperty("currentCollection", quickListCollection);

					// Load new layout if available
					// check if collection's assets are in a different theme
					std::string layoutName;
					config_.getProperty("collections." + quickListCollection + ".layout", layoutName);
					if (layoutName == "")
					{
						config_.getProperty(OPTION_LAYOUT, layoutName);
					}
					PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);

					bool defaultToCurrentLayout = false;
					if (std::string quickListPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(quickListPrefix + "defaultToCurrentLayout"))
					{
						config_.getProperty(quickListPrefix + "defaultToCurrentLayout", defaultToCurrentLayout);
					}

					// try collection name
					Page* page = pb.buildPage(quickListCollection, defaultToCurrentLayout);
					if (page)
					{
						if (page->controlsType() != currentPage_->controlsType())
						{
							updatePageControls(page->controlsType());
						}
						currentPage_->freeGraphicsMemory();
						pages_.push(currentPage_);
						currentPage_ = page;
						currentPage_->setLocked(kioskLock_);
						CollectionInfo* info = getCollection(quickListCollection);
						if (info == nullptr)
						{
							setState(RETROFE_BACK_MENU_LOAD_ART);
							break;
						}
						currentPage_->pushCollection(info);
						cycleVector_.clear();
					}
					else
					{
						LOG_ERROR("RetroFE", "Could not create page");
					}
					// currentPage_->reallocateMenuSpritePoints(); // update menu
				}
				std::string selectPlaylist = quickListPlaylist;
				if (quickListPlaylist == "")
				{
					std::string autoPlaylist = "quicklist";
					if (std::string quickListPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(quickListPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(quickListPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					quickListPlaylist = autoPlaylist;
				}
				currentPage_->selectPlaylist(selectPlaylist);
				currentPage_->onNewItemSelected();
				// refresh menu if in different collection
				if (quickListCollection != "" && quickListCollection != collectionName)
				{
					currentPage_->reallocateMenuSpritePoints();
				}
			}
			break;
			case RETROFE_SETTINGS_PAGE_REQUEST:
			if (currentPage_->isIdle() && currentPage_->getCollectionName() != "")
			{
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				if (currentPage_->getPlaylistName() != quickListCollectionPlaylist)
				{
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				}
				config_.setProperty("lastCollection", collectionName);
				setState(RETROFE_PLAYLIST_REQUEST);
				if (settingsCollection != "" && settingsCollection != collectionName)
				{
					setState(RETROFE_NEXT_PAGE_MENU_LOAD_ART);

					// update new current collection
					cycleVector_.clear();
					config_.setProperty("currentCollection", settingsCollection);

					// Load new layout if available
					// check if collection's assets are in a different theme
					std::string layoutName;
					config_.getProperty("collections." + settingsCollection + ".layout", layoutName);
					if (layoutName == "")
					{
						config_.getProperty(OPTION_LAYOUT, layoutName);
					}
					PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);

					bool defaultToCurrentLayout = false;
					if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(settingPrefix + "defaultToCurrentLayout"))
					{
						config_.getProperty(settingPrefix + "defaultToCurrentLayout", defaultToCurrentLayout);
					}

					// try collection name
					Page* page = pb.buildPage(settingsCollection, defaultToCurrentLayout);
					if (page)
					{
						if (page->controlsType() != currentPage_->controlsType())
						{
							updatePageControls(page->controlsType());
						}
						currentPage_->freeGraphicsMemory();
						pages_.push(currentPage_);
						currentPage_ = page;
						currentPage_->setLocked(kioskLock_);
						CollectionInfo* info = getCollection(settingsCollection);
						if (info == nullptr)
						{
							setState(RETROFE_BACK_MENU_LOAD_ART);
							break;
						}
						currentPage_->pushCollection(info);
						cycleVector_.clear();
					}
					else
					{
						LOG_ERROR("RetroFE", "Could not create page");
					}
					// currentPage_->reallocateMenuSpritePoints(); // update menu
				}
				std::string selectPlaylist = settingsPlaylist;
				if (settingsPlaylist == "")
				{
					std::string autoPlaylist = "settings";
					if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					selectPlaylist = autoPlaylist;
				}
				currentPage_->selectPlaylist(selectPlaylist);
				currentPage_->onNewItemSelected();
				// refresh menu if in different collection
				if (settingsCollection != "" && settingsCollection != collectionName)
				{
					currentPage_->reallocateMenuSpritePoints();
				}
			}
			break;
			// Next page; start onMenuExit animation
			case RETROFE_NEXT_PAGE_REQUEST:
			currentPage_->exitMenu();
			setState(RETROFE_NEXT_PAGE_MENU_EXIT);
			break;

			// Wait for onMenuExit animation to finish; load new page if applicable; load art
			case RETROFE_NEXT_PAGE_MENU_EXIT:
			if (currentPage_->isIdle())
			{
				setState(RETROFE_NEXT_PAGE_MENU_LOAD_ART);

				const std::string nextPageName = nextPageItem_ ? nextPageItem_->name : "";
				const std::string collectionName = currentPage_->getCollectionName();

				if (currentPage_->getSelectedItem())
					l.LEDBlinky(8, currentPage_->getSelectedItem()->name, currentPage_->getSelectedItem());

				CollectionInfo* info = currentPage_->getCollection();

				if (collectionName != nextPageName)
				{
					lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();

					info = menuMode_ ? getMenuCollection(nextPageName) : getCollection(nextPageName);
					if (!info)
					{
						LOG_ERROR("RetroFE", "Collection not found with Name " + nextPageName);
						setState(RETROFE_BACK_MENU_LOAD_ART);
						break;
					}
				}

				if (!menuMode_)
				{
					std::string layoutName;
					config_.getProperty("collections." + nextPageName + ".layout", layoutName);
					if (layoutName.empty())
						config_.getProperty(OPTION_LAYOUT, layoutName);

					PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);
					bool defaultToCurrentLayout = false;

					std::string settingPrefix = "collections." + collectionName + ".";
					config_.getProperty(settingPrefix + "defaultToCurrentLayout", defaultToCurrentLayout);

					Page* page = pb.buildPage(nextPageName, defaultToCurrentLayout);
					if (!page)
					{
						LOG_ERROR("RetroFE", "Could not create page for " + nextPageName);
						setState(RETROFE_BACK_MENU_LOAD_ART);
						break;
					}

					if (page->controlsType() != currentPage_->controlsType())
						updatePageControls(page->controlsType());

					currentPage_->freeGraphicsMemory();
					pages_.push(currentPage_);
					currentPage_ = page;
					currentPage_->setLocked(kioskLock_);
				}

				cycleVector_.clear();
				config_.setProperty("currentCollection", nextPageName);

				currentPage_->pushCollection(info);

				std::string autoPlaylist = "all";
				std::string newSettingPrefix = "collections." + currentPage_->getCollectionName() + ".";
				if (!config_.getProperty(newSettingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist))
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);

				if (currentPage_->getCollectionName() == "Favorites")
					autoPlaylist = "favorites";

				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);

				auto playlistIt = lastMenuPlaylists_.find(nextPageName);
				bool returnToRemembered = rememberMenu && playlistIt != lastMenuPlaylists_.end();

				if (returnToRemembered)
				{
					currentPage_->selectPlaylist(playlistIt->second);

					auto offsetIt = lastMenuOffsets_.find(nextPageName);
					if (offsetIt != lastMenuOffsets_.end())
						currentPage_->setScrollOffsetIndex(offsetIt->second);
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints(); // update playlist menu

				if (currentPage_->getCollectionSize() == 0)
				{
					bool backOnEmpty = false;
					config_.getProperty(OPTION_BACKONEMPTY, backOnEmpty);
					if (backOnEmpty)
						setState(RETROFE_BACK_MENU_EXIT);
				}
			}
			break;


			// Start onMenuEnter animation
			case RETROFE_NEXT_PAGE_MENU_LOAD_ART:
			if (currentPage_->getMenuDepth() != 1)
			{
				currentPage_->enterMenu();
			}
			else
			{
				currentPage_->start();
			}
			if (currentPage_->getSelectedItem())
				l.LEDBlinky(9, currentPage_->getSelectedItem()->collectionInfo->name, currentPage_->getSelectedItem());
			setState(RETROFE_NEXT_PAGE_MENU_ENTER);
			break;

			// Wait for onMenuEnter animation to finish
			case RETROFE_NEXT_PAGE_MENU_ENTER:
			if (currentPage_->isIdle())
			{
				inputClear = false;
				config_.getProperty(OPTION_COLLECTIONINPUTCLEAR, inputClear);
				if (inputClear)
				{
					// Empty event queue
					SDL_Event e;
					while (SDL_PollEvent(&e))
						input_.update(e);
					input_.resetStates();
				}
				setState(RETROFE_IDLE);
			}
			break;

			// Start exit animation
			case RETROFE_COLLECTION_DOWN_REQUEST:
			// Inside a collection with a different layout
			if (!pages_.empty() && currentPage_->getMenuDepth() == 1)
			{
				currentPage_->stop();
				m.clearPage();
				menuMode_ = false;
				setState(RETROFE_COLLECTION_DOWN_EXIT);
			}
			// Inside a collection with the same layout
			else if (currentPage_->getMenuDepth() > 1)
			{
				currentPage_->exitMenu();
				setState(RETROFE_COLLECTION_DOWN_EXIT);
			}
			// Not in a collection
			else
			{
				setState(RETROFE_COLLECTION_DOWN_ENTER);
				// Check playlist change in attract mode
				if (attractMode_)
				{
					attractModePlaylistCollectionNumber_ += 1;
					int attractModePlaylistCollectionNumber = 0;
					config_.getProperty("attractModePlaylistCollectionNumber", attractModePlaylistCollectionNumber);
					// Check if playlist should be changed
					if (attractModePlaylistCollectionNumber_ > 0 &&
						attractModePlaylistCollectionNumber_ >= attractModePlaylistCollectionNumber)
					{
						attractModePlaylistCollectionNumber_ = 0;
						currentPage_->nextPlaylist();

						if (isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()))
						{
							// todo find next playlist that isn't in skip list
							currentPage_->nextPlaylist();
						}

						setState(RETROFE_PLAYLIST_REQUEST);
					}
				}
			}
			break;

			// Wait for the menu exit animation to finish
			case RETROFE_COLLECTION_DOWN_EXIT:
			if (currentPage_->isIdle())
			{
				// remember current collection and playlist
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				// Inside a collection with a different layout
				if (currentPage_->getMenuDepth() == 1)
				{
					currentPage_->deInitialize();
					delete currentPage_;
					currentPage_ = pages_.top();
					pages_.pop();
					currentPage_->allocateGraphicsMemory();
					currentPage_->setLocked(kioskLock_);
				}
				// Inside a collection with the same layout
				else
				{
					currentPage_->popCollection();
				}

				// update new current collection
				cycleVector_.clear();
				collectionName = currentPage_->getCollectionName();
				config_.setProperty("currentCollection", collectionName);
				// check collection for setting
				std::string autoPlaylist = "all";
				if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
				{
					config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
				}
				else
				{
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
				}

				if (currentPage_->getCollectionName() == "Favorites")
				{
					autoPlaylist = "favorites";
				}

				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
				bool returnToRememberedPlaylist =
					rememberMenu && lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end();
				if (returnToRememberedPlaylist)
				{
					currentPage_->selectPlaylist(lastMenuPlaylists_[collectionName]); // Switch to last playlist
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				if (returnToRememberedPlaylist)
				{
					if (lastMenuOffsets_.size() && lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end())
					{
						currentPage_->setScrollOffsetIndex(lastMenuOffsets_[collectionName]);
					}
				}

				setState(RETROFE_COLLECTION_DOWN_MENU_ENTER);
				currentPage_->onNewItemSelected();

				// Check playlist change in attract mode
				if (attractMode_)
				{
					attractModePlaylistCollectionNumber_ += 1;
					int attractModePlaylistCollectionNumber = 0;
					config_.getProperty("attractModePlaylistCollectionNumber", attractModePlaylistCollectionNumber);
					// Check if playlist should be changed
					if (attractModePlaylistCollectionNumber_ > 0 &&
						attractModePlaylistCollectionNumber_ >= attractModePlaylistCollectionNumber)
					{
						attractModePlaylistCollectionNumber_ = 0;
						currentPage_->nextPlaylist();
						if (isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()))
						{
							// todo find next playlist that isn't in skip list
							currentPage_->nextPlaylist();
						}
						setState(RETROFE_PLAYLIST_REQUEST);
					}
				}
			}
			break;

			// Start menu enter animation
			case RETROFE_COLLECTION_DOWN_MENU_ENTER:
			currentPage_->enterMenu();
			setState(RETROFE_COLLECTION_DOWN_ENTER);
			break;

			// Waiting for enter animation to stop
			case RETROFE_COLLECTION_DOWN_ENTER:
			if (currentPage_->isIdle())
			{
				int attractModePlaylistCollectionNumber = 0;
				config_.getProperty("attractModePlaylistCollectionNumber", attractModePlaylistCollectionNumber);
				if (!(attractMode_ && attractModePlaylistCollectionNumber > 0 &&
					attractModePlaylistCollectionNumber_ == 0))
				{
					currentPage_->setScrolling(Page::ScrollDirectionForward);
					currentPage_->scroll(true, false);
					currentPage_->updateScrollPeriod();
				}
				setState(RETROFE_COLLECTION_DOWN_SCROLL);
			}
			break;

			// Waiting for scrolling animation to stop
			case RETROFE_COLLECTION_DOWN_SCROLL:
			if (currentPage_->isMenuIdle())
			{
				std::string attractModeSkipCollection = "";
				config_.getProperty(OPTION_ATTRACTMODESKIPCOLLECTION, attractModeSkipCollection);
				// Check if we need to skip this collection in attract mode or if we can select it
				if (attractMode_ && currentPage_->getSelectedItem()->name == attractModeSkipCollection)
				{
					currentPage_->setScrolling(Page::ScrollDirectionForward);
					currentPage_->scroll(true, false);
					currentPage_->updateScrollPeriod();
				}
				else
				{
					RETROFE_STATE state_tmp = processUserInput(currentPage_);
					if (state_tmp == RETROFE_COLLECTION_DOWN_REQUEST)
					{
						setState(RETROFE_COLLECTION_DOWN_REQUEST);
					}
					else if (state_tmp == RETROFE_COLLECTION_UP_REQUEST)
					{
						setState(RETROFE_COLLECTION_UP_REQUEST);
					}
					else
					{
						currentPage_->setScrolling(Page::ScrollDirectionIdle); // Stop scrolling
						nextPageItem_ = currentPage_->getSelectedItem();

						bool enterOnCollection = true;
						config_.getProperty(OPTION_ENTERONCOLLECTION, enterOnCollection);
						if (nextPageItem_->leaf ||
							(!attractMode_ &&
								!enterOnCollection)) // Current selection is a game or enterOnCollection is not set
						{
							setState(RETROFE_HIGHLIGHT_REQUEST);
						}
						else // Current selection is a menu
						{
							setState(RETROFE_COLLECTION_HIGHLIGHT_REQUEST);
						}
					}
				}
			}
			break;

			// Start onHighlightExit animation
			case RETROFE_COLLECTION_HIGHLIGHT_REQUEST:
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			currentPage_->highlightExit();

			setState(RETROFE_COLLECTION_HIGHLIGHT_EXIT);
			break;

			// Wait for onHighlightExit animation to finish; load art
			case RETROFE_COLLECTION_HIGHLIGHT_EXIT:
			if (currentPage_->isIdle())
			{
				currentPage_->highlightLoadArt();
				setState(RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART);
			}
			break;

			// Start onHighlightEnter animation
			case RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART:
			currentPage_->highlightEnter();
			if (currentPage_->getSelectedItem())
				l.LEDBlinky(9, currentPage_->getSelectedItem()->collectionInfo->name, currentPage_->getSelectedItem());
			setState(RETROFE_COLLECTION_HIGHLIGHT_ENTER);
			break;

			// Wait for onHighlightEnter animation to finish
			case RETROFE_COLLECTION_HIGHLIGHT_ENTER:
			if (currentPage_->isIdle())
			{
				nextPageItem_ = currentPage_->getSelectedItem();

				RETROFE_STATE state_tmp = processUserInput(currentPage_);
				if (state_tmp == RETROFE_COLLECTION_DOWN_REQUEST)
				{
					setState(RETROFE_COLLECTION_DOWN_REQUEST);
				}
				else if (state_tmp == RETROFE_COLLECTION_UP_REQUEST)
				{
					setState(RETROFE_COLLECTION_UP_REQUEST);
				}
				else
				{
					setState(RETROFE_NEXT_PAGE_REQUEST);
				}
			}
			break;

			// Start exit animation
			case RETROFE_COLLECTION_UP_REQUEST:
			if (!pages_.empty() && currentPage_->getMenuDepth() == 1) // Inside a collection with a different layout
			{
				currentPage_->stop();
				m.clearPage();
				menuMode_ = false;
				setState(RETROFE_COLLECTION_UP_EXIT);
			}
			else if (currentPage_->getMenuDepth() > 1) // Inside a collection with the same layout
			{
				currentPage_->exitMenu();
				setState(RETROFE_COLLECTION_UP_EXIT);
			}
			else // Not in a collection
			{
				setState(RETROFE_COLLECTION_UP_ENTER);
			}
			break;

			// Wait for the menu exit animation to finish
			case RETROFE_COLLECTION_UP_EXIT:
			if (currentPage_->isIdle())
			{
				// remember current collection and playlist
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				if (currentPage_->getMenuDepth() == 1) // Inside a collection with a different layout
				{
					currentPage_->deInitialize();
					delete currentPage_;
					currentPage_ = pages_.top();
					pages_.pop();
					currentPage_->allocateGraphicsMemory();
					currentPage_->setLocked(kioskLock_);
				}
				else // Inside a collection with the same layout
				{
					currentPage_->popCollection();
				}

				// update new current collection
				cycleVector_.clear();
				collectionName = currentPage_->getCollectionName();
				config_.setProperty("currentCollection", collectionName);

				// check collection for setting
				std::string autoPlaylist = "all";
				if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
				{
					config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
				}
				else
				{
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
				}

				if (currentPage_->getCollectionName() == "Favorites")
				{
					autoPlaylist = "favorites";
				}

				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
				bool returnToRememberedPlaylist =
					rememberMenu && lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end();
				if (returnToRememberedPlaylist)
				{
					currentPage_->selectPlaylist(lastMenuPlaylists_[collectionName]); // Switch to last playlist
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				if (returnToRememberedPlaylist)
				{
					if (lastMenuOffsets_.size() && lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end())
					{
						currentPage_->setScrollOffsetIndex(lastMenuOffsets_[collectionName]);
					}
				}

				currentPage_->onNewItemSelected();
				setState(RETROFE_COLLECTION_UP_MENU_ENTER);
			}
			break;

			// Start menu enter animation
			case RETROFE_COLLECTION_UP_MENU_ENTER:
			currentPage_->enterMenu();
			setState(RETROFE_COLLECTION_UP_ENTER);
			break;

			// Waiting for enter animation to stop
			case RETROFE_COLLECTION_UP_ENTER:
			if (currentPage_->isIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionBack);
				currentPage_->scroll(false, false);
				currentPage_->updateScrollPeriod();
				setState(RETROFE_COLLECTION_UP_SCROLL);
			}
			break;

			// Waiting for scrolling animation to stop
			case RETROFE_COLLECTION_UP_SCROLL:
			if (currentPage_->isMenuIdle())
			{
				RETROFE_STATE state_tmp;
				state_tmp = processUserInput(currentPage_);
				if (state_tmp == RETROFE_COLLECTION_DOWN_REQUEST)
				{
					setState(RETROFE_COLLECTION_DOWN_REQUEST);
				}
				else if (state_tmp == RETROFE_COLLECTION_UP_REQUEST)
				{
					setState(RETROFE_COLLECTION_UP_REQUEST);
				}
				else
				{
					currentPage_->setScrolling(Page::ScrollDirectionIdle); // Stop scrolling
					nextPageItem_ = currentPage_->getSelectedItem();
					bool enterOnCollection = true;
					config_.getProperty(OPTION_ENTERONCOLLECTION, enterOnCollection);
					// Current selection is a game or enterOnCollection is not set
					if (currentPage_->getSelectedItem()->leaf || !enterOnCollection)
					{
						setState(RETROFE_HIGHLIGHT_REQUEST);
					}
					// Current selection is a menu
					else
					{
						setState(RETROFE_COLLECTION_HIGHLIGHT_EXIT);
					}
				}
			}
			break;

			// Launching a menu entry
			case RETROFE_HANDLE_MENUENTRY:
			// Empty event queue
			SDL_Event e;
			while (SDL_PollEvent(&e))
				input_.update(e);
			input_.resetStates();

			// Handle menu entry
			m.handleEntry(currentPage_->getSelectedItem());

			// Empty event queue
			while (SDL_PollEvent(&e))
				input_.update(e);
			input_.resetStates();

			setState(RETROFE_IDLE);
			break;

			case RETROFE_ATTRACT_LAUNCH_ENTER:
			if (currentPage_->isIdle())
			{
				currentPage_->setSelectedItem();
				currentPage_->onNewItemSelected();
				currentPage_->enterGame();  // Start onGameEnter animation
				currentPage_->playSelect(); // Play launch sound
				setState(RETROFE_ATTRACT_LAUNCH_REQUEST);
			}

			break;
			case RETROFE_ATTRACT_LAUNCH_REQUEST:
			if (currentPage_->isIdle())
			{
				nextPageItem_ = currentPage_->getSelectedItem();
				launchEnter();

				l.LEDBlinky(3, nextPageItem_->collectionInfo->name, nextPageItem_);
				// Run and check if we need to reboot
				if (l.run(nextPageItem_->collectionInfo->name, nextPageItem_, currentPage_, true))
				{
					attract_.reset();
					// Run launchExit function when unloadSDL flag is set
					bool unloadSDL = false;
					config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
					if (unloadSDL)
					{
						launchExit();
					}
					reboot_ = true;
					setState(RETROFE_QUIT_REQUEST);
				}
				else
				{
					launchExit();
					l.LEDBlinky(4);
					currentPage_->exitGame();

					setState(RETROFE_LAUNCH_EXIT);
				}
			}
			break;

			// Launching game; start onGameEnter animation
			case RETROFE_LAUNCH_ENTER:
			if (currentPage_->isMenuScrolling())
			{
				setState(RETROFE_IDLE);
			}
			else
			{
				currentPage_->enterGame();  // Start onGameEnter animation
				currentPage_->playSelect(); // Play launch sound
				setState(RETROFE_LAUNCH_REQUEST);
			}
			break;

			// Wait for onGameEnter animation to finish; launch game; start onGameExit animation
			case RETROFE_LAUNCH_REQUEST:
			if (currentPage_->isIdle())
			{
				nextPageItem_ = currentPage_->getSelectedItem();
				launchEnter();
				CollectionInfoBuilder cib(config_, *metadb_);
				std::string lastPlayedSkipCollection = "";
				int size = 0;
				config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
				config_.getProperty(OPTION_LASTPLAYEDSIZE, size);

				nextPageItem_ = currentPage_->getSelectedItem();

				if (lastPlayedSkipCollection != "")
				{
					// Check if current collection is in the skip list
					std::stringstream ss(lastPlayedSkipCollection);
					std::string collection = "";
					bool updateLastPlayed = true;
					while (ss.good())
					{
						getline(ss, collection, ',');
						if (nextPageItem_->collectionInfo->name == collection)
						{
							updateLastPlayed = false;
							break;
						}
					}
					if (updateLastPlayed)
					{
						cib.updateLastPlayedPlaylist(currentPage_->getCollection(), nextPageItem_, size);
						currentPage_->updateReloadables(0);
					}
				}

				l.LEDBlinky(3, nextPageItem_->collectionInfo->name, nextPageItem_);
				// Run and check if we need to reboot
				if (l.run(nextPageItem_->collectionInfo->name, nextPageItem_, currentPage_))
				{
					attract_.reset();
					bool unloadSDL = false;
					config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
					if (unloadSDL)
					{
						launchExit();
					}
					reboot_ = true;
					setState(RETROFE_QUIT_REQUEST);
				}
				else
				{
					attract_.reset();
					l.LEDBlinky(4);
					currentPage_->exitGame();
					if (currentPage_->getPlaylistName() == "lastplayed")
					{
						currentPage_->setScrollOffsetIndex(0);
						currentPage_->reallocateMenuSpritePoints();
					}
					launchExit();
					setState(RETROFE_LAUNCH_EXIT);
				}
			}
			break;

			// Wait for onGameExit animation to finish
			case RETROFE_LAUNCH_EXIT: {
				// Only update `state` if `currentPage_` is idle
				if (currentPage_->isIdle()) {
					setState(RETROFE_IDLE);
				}
				break;
			}


									// Go back a page; start onMenuExit animation
			case RETROFE_BACK_REQUEST:
			if (currentPage_->getMenuDepth() == 1)
			{
				currentPage_->stop();
				m.clearPage();
				menuMode_ = false;
			}
			else
			{
				currentPage_->exitMenu();
			}
			setState(RETROFE_BACK_MENU_EXIT);
			break;

			// Wait for onMenuExit animation to finish; load previous page; load art
			case RETROFE_BACK_MENU_EXIT:
			if (currentPage_->isIdle()) {
				// Store current state before transition
				std::string collectionName = currentPage_->getCollectionName();
				if (!collectionName.empty()) {
					lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				}

				// Handle different layout vs same layout cases
				if (currentPage_->getMenuDepth() == 1 && !pages_.empty()) {
					// Different layout case - need to switch pages
					Page* prevPage = pages_.top();
					pages_.pop();

					// Validate transition target
					if (!prevPage) {
						LOG_ERROR("RetroFE", "Invalid previous page in stack");
						setState(RETROFE_IDLE);
						break;
					}

					// Check if we need to update controls for the previous page
					if (prevPage->controlsType() != currentPage_->controlsType()) {
						updatePageControls(prevPage->controlsType());
					}

					// Cleanup current page
					currentPage_->deInitialize();
					delete currentPage_;
					currentPage_ = prevPage;

					// Setup restored page
					if (!currentPage_->getSelectedItem()) {
						LOG_ERROR("RetroFE", "Invalid page state after restoration");
						setState(RETROFE_QUIT_REQUEST);
						break;
					}

					currentPage_->allocateGraphicsMemory();
					currentPage_->setLocked(kioskLock_);
					currentPage_->resume();  //  This was missing
				}
				else {
					// Same layout case - just pop collection
					if (!currentPage_->popCollection()) {
						LOG_ERROR("RetroFE", "Failed to pop collection during back navigation");
						setState(RETROFE_IDLE);
						break;
					}
				}

				// Update collection cycle if active
				cycleVector_.clear();
				collectionName = currentPage_->getCollectionName();
				config_.setProperty("currentCollection", collectionName);

				// Get playlist configuration
				std::string autoPlaylist = "all";
				if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST)) {
					config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
				}
				else {
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
				}

				// Handle Favorites collection special case
				if (currentPage_->getCollectionName() == "Favorites") {
					autoPlaylist = "favorites";
				}

				// Check if we should return to remembered playlist
				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
				bool returnToRememberedPlaylist = rememberMenu &&
					lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end();

				// Set appropriate playlist
				if (returnToRememberedPlaylist) {
					currentPage_->selectPlaylist(lastMenuPlaylists_[collectionName]);

					// Restore previous offset if available
					if (lastMenuOffsets_.find(collectionName) != lastMenuOffsets_.end()) {
						currentPage_->setScrollOffsetIndex(lastMenuOffsets_[collectionName]);
					}
				}
				else {
					// Use auto playlist with fallback to "all"
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist) {
						currentPage_->selectPlaylist("all");
					}
				}

				// Update display
				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints(); // update playlist menu

				setState(RETROFE_BACK_MENU_LOAD_ART);
			}
			break;

			// Start onMenuEnter animation
			case RETROFE_BACK_MENU_LOAD_ART:
			currentPage_->enterMenu();
			setState(RETROFE_BACK_MENU_ENTER);
			break;

			// Wait for onMenuEnter animation to finish
			case RETROFE_BACK_MENU_ENTER:
			if (currentPage_->isIdle())
			{
				bool collectionInputClear = false;
				config_.getProperty(OPTION_COLLECTIONINPUTCLEAR, collectionInputClear);
				if (collectionInputClear)
				{
					// Empty event queue
					SDL_Event e;
					while (SDL_PollEvent(&e))
						input_.update(e);
					input_.resetStates();
				}
				setState(RETROFE_IDLE);
			}
			break;

			// Start menu mode
			case RETROFE_MENUMODE_START_REQUEST:
			if (currentPage_->isIdle())
			{
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				// check if collection's assets are in a different theme
				std::string layoutName;
				config_.getProperty("collections." + collectionName + ".layout", layoutName);
				if (layoutName == "")
				{
					config_.getProperty(OPTION_LAYOUT, layoutName);
				}
				PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_, true);
				if (Page* page = pb.buildPage())
				{
					if (page->controlsType() != currentPage_->controlsType())
					{
						updatePageControls(page->controlsType());
					}
					currentPage_->freeGraphicsMemory();
					pages_.push(currentPage_);
					currentPage_ = page;
					currentPage_->setLocked(kioskLock_);
					menuMode_ = true;
					m.setPage(page);
				}
				else
				{
					LOG_ERROR("RetroFE", "Could not create page");
				}

				// update new current collection
				cycleVector_.clear();
				config_.setProperty("currentCollection", "menu");
				currentPage_->pushCollection(getMenuCollection("menu"));

				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints(); // update playlist menu

				setState(RETROFE_MENUMODE_START_LOAD_ART);
			}
			break;

			case RETROFE_MENUMODE_START_LOAD_ART:
			currentPage_->start();
			setState(RETROFE_MENUMODE_START_ENTER);
			break;

			case RETROFE_MENUMODE_START_ENTER:
			if (currentPage_->isIdle())
			{
				SDL_Event e;
				while (SDL_PollEvent(&e))
					input_.update(e);
				input_.resetStates();
				setState(RETROFE_IDLE);
			}
			break;

			// Wait for splash mode animation to finish
			case RETROFE_NEW:
			if (currentPage_->isIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;

			// Start the onExit animation
			case RETROFE_QUIT_REQUEST:
			currentPage_->stop();
			setState(RETROFE_QUIT);
			break;

			// Wait for onExit animation to finish before quitting RetroFE
			case RETROFE_QUIT:
			if (currentPage_->isGraphicsIdle())
			{
				l.LEDBlinky(2);
				l.exitScript();
				running = false;
			}
			break;
		}


		// Handle screen updates and attract mode
		if (running)
		{
			if (currentPage_)
			{
				if (!splashMode && !paused_)
				{
					float attract_dt = static_cast<float>(deltaTime);
					const float MAX_REASONABLE_DELTA_TIME = 0.1f; // 100ms, or 1/10th of a second
					if (attract_dt > MAX_REASONABLE_DELTA_TIME) {
						attract_dt = MAX_REASONABLE_DELTA_TIME;
					}
					int attractReturn = attract_.update(attract_dt, *currentPage_);
					if (!kioskLock_ && attractReturn == 1) // Change playlist
					{
						attract_.reset(attract_.isSet());

						bool attractModeCyclePlaylist = getAttractModeCyclePlaylist();
						if (attractModeCyclePlaylist)
							currentPage_->nextCyclePlaylist(getPlaylistCycle());
						else
							currentPage_->nextPlaylist();

						// if that next playlist is one to skip for attract, then find one that isn't
						if (isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()))
						{
							if (attractModeCyclePlaylist)
							{
								goToNextAttractModePlaylistByCycle(getPlaylistCycle());
							}
							else
							{
								// todo perform smarter playlist skipping
								currentPage_->nextPlaylist();
							}
						}
						setState(RETROFE_PLAYLIST_REQUEST);
					}
					if (!kioskLock_ && attractReturn == 2) // Change collection
					{
						attract_.reset(attract_.isSet());
						setState(RETROFE_COLLECTION_DOWN_REQUEST);
					}
					if (attractModeLaunch && !kioskLock_ && attractReturn == 3) {
						attract_.reset(attract_.isSet());
						setState(RETROFE_ATTRACT_LAUNCH_ENTER);
					}
				}
				if (menuMode_)
				{
					attract_.reset();
				}
				currentPage_->update(deltaTime);
				SDL_PumpEvents();
				// Update keystate at 30Hz
				if (currentTime_ - lastInputUpdateTime >= inputUpdateInterval)
				{
					input_.updateKeystate();
					lastInputUpdateTime = currentTime_;
				}
				if (!splashMode && !paused_)
				{
					if (currentPage_->isAttractIdle())
					{
						if (!attractMode_ && attract_.isSet())
						{
							// hide toggle before attract mode
							if (buildInfo_ || collectionInfo_ || gameInfo_)
							{
								resetInfoToggle();
							}
							else
							{
								currentPage_->attractEnter();
								l.LEDBlinky(5);
							}
						}
						else if (attractMode_ && !attract_.isSet())
						{
							currentPage_->attractExit();
							l.LEDBlinky(6);
						}
						else if (attract_.isSet())
						{
							currentPage_->attract();
						}
						attractMode_ = attract_.isSet();
					}
				}
			}

			render();

			bool activelyAnimating = isUserActive(currentTime_)
				|| currentPage_->isMenuScrolling()
				|| !currentPage_->isIdle()
				|| !currentPage_->isGraphicsIdle()
				|| currentPage_->isPlaylistScrolling()
				|| currentPage_->isGamesScrolling();

			double currentFrameIntervalMs = activelyAnimating ? fpsTime : fpsIdleTime;

			// Advance nextFrameTime by one interval FROM ITS CURRENT VALUE
			// This nextFrameTime now represents the target END time for the current frame (after sleeping)
			nextFrameTime += currentFrameIntervalMs;

			// Get current time right before sleep
			double timeBeforeSleepMs = SDL_GetPerformanceCounter() * 1000.0 / freq_;

			// Calculate sleep duration
			double sleepTimeMs = nextFrameTime - timeBeforeSleepMs;
			//Clamp and advance the target frame time

			if (!vSync && sleepTimeMs > 0.0) {
				Utils::preciseSleep(sleepTimeMs / 1000.0);
				uint64_t ultimateTargetTicks = (uint64_t)(nextFrameTime * freq_ / 1000.0);
				while (SDL_GetPerformanceCounter() < ultimateTargetTicks);
			}

			//Measure how long the full frame took (update + render + sleep)
			uint64_t loopEnd = SDL_GetPerformanceCounter();
			double actualTotalFrameDurationMs = (loopEnd - loopStart) * 1000.0 / freq_; // loopStart was actual start
			lastFrameTimeMs_ = actualTotalFrameDurationMs; // For display
		}
	}

	return reboot_;
}

void RetroFE::setState(RETROFE_STATE newState) {
	if (newState != state_) {
		LOG_DEBUG("RetroFE", "Transitioning from " + stateToString(state_) + " to " + stateToString(newState));
	}
	state_ = newState;
}

RetroFE::RETROFE_STATE RetroFE::getState() const {
	return state_;
}

std::string RetroFE::stateToString(RETROFE_STATE s) const {
	switch (s) {
		case RETROFE_IDLE: return "RETROFE_IDLE";
		case RETROFE_LOAD_ART: return "RETROFE_LOAD_ART";
		case RETROFE_ENTER: return "RETROFE_ENTER";
		case RETROFE_SPLASH_EXIT: return "RETROFE_SPLASH_EXIT";
		case RETROFE_PLAYLIST_NEXT: return "RETROFE_PLAYLIST_NEXT";
		case RETROFE_PLAYLIST_PREV: return "RETROFE_PLAYLIST_PREV";
		case RETROFE_PLAYLIST_NEXT_CYCLE: return "RETROFE_PLAYLIST_NEXT_CYCLE";
		case RETROFE_PLAYLIST_PREV_CYCLE: return "RETROFE_PLAYLIST_PREV_CYCLE";
		case RETROFE_PLAYLIST_REQUEST: return "RETROFE_PLAYLIST_REQUEST";
		case RETROFE_PLAYLIST_EXIT: return "RETROFE_PLAYLIST_EXIT";
		case RETROFE_PLAYLIST_LOAD_ART: return "RETROFE_PLAYLIST_LOAD_ART";
		case RETROFE_PLAYLIST_ENTER: return "RETROFE_PLAYLIST_ENTER";
		case RETROFE_MENUJUMP_REQUEST: return "RETROFE_MENUJUMP_REQUEST";
		case RETROFE_MENUJUMP_EXIT: return "RETROFE_MENUJUMP_EXIT";
		case RETROFE_MENUJUMP_LOAD_ART: return "RETROFE_MENUJUMP_LOAD_ART";
		case RETROFE_MENUJUMP_ENTER: return "RETROFE_MENUJUMP_ENTER";
		case RETROFE_HIGHLIGHT_REQUEST: return "RETROFE_HIGHLIGHT_REQUEST";
		case RETROFE_HIGHLIGHT_EXIT: return "RETROFE_HIGHLIGHT_EXIT";
		case RETROFE_HIGHLIGHT_LOAD_ART: return "RETROFE_HIGHLIGHT_LOAD_ART";
		case RETROFE_HIGHLIGHT_ENTER: return "RETROFE_HIGHLIGHT_ENTER";
		case RETROFE_NEXT_PAGE_REQUEST: return "RETROFE_NEXT_PAGE_REQUEST";
		case RETROFE_NEXT_PAGE_MENU_EXIT: return "RETROFE_NEXT_PAGE_MENU_EXIT";
		case RETROFE_NEXT_PAGE_MENU_LOAD_ART: return "RETROFE_NEXT_PAGE_MENU_LOAD_ART";
		case RETROFE_NEXT_PAGE_MENU_ENTER: return "RETROFE_NEXT_PAGE_MENU_ENTER";
		case RETROFE_COLLECTION_UP_REQUEST: return "RETROFE_COLLECTION_UP_REQUEST";
		case RETROFE_COLLECTION_UP_EXIT: return "RETROFE_COLLECTION_UP_EXIT";
		case RETROFE_COLLECTION_UP_MENU_ENTER: return "RETROFE_COLLECTION_UP_MENU_ENTER";
		case RETROFE_COLLECTION_UP_ENTER: return "RETROFE_COLLECTION_UP_ENTER";
		case RETROFE_COLLECTION_UP_SCROLL: return "RETROFE_COLLECTION_UP_SCROLL";
		case RETROFE_COLLECTION_HIGHLIGHT_REQUEST: return "RETROFE_COLLECTION_HIGHLIGHT_REQUEST";
		case RETROFE_COLLECTION_HIGHLIGHT_EXIT: return "RETROFE_COLLECTION_HIGHLIGHT_EXIT";
		case RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART: return "RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART";
		case RETROFE_COLLECTION_HIGHLIGHT_ENTER: return "RETROFE_COLLECTION_HIGHLIGHT_ENTER";
		case RETROFE_COLLECTION_DOWN_REQUEST: return "RETROFE_COLLECTION_DOWN_REQUEST";
		case RETROFE_COLLECTION_DOWN_EXIT: return "RETROFE_COLLECTION_DOWN_EXIT";
		case RETROFE_COLLECTION_DOWN_MENU_ENTER: return "RETROFE_COLLECTION_DOWN_MENU_ENTER";
		case RETROFE_COLLECTION_DOWN_ENTER: return "RETROFE_COLLECTION_DOWN_ENTER";
		case RETROFE_COLLECTION_DOWN_SCROLL: return "RETROFE_COLLECTION_DOWN_SCROLL";
		case RETROFE_HANDLE_MENUENTRY: return "RETROFE_HANDLE_MENUENTRY";
		case RETROFE_ATTRACT_LAUNCH_ENTER: return "RETROFE_ATTRACT_LAUNCH_ENTER";
		case RETROFE_ATTRACT_LAUNCH_REQUEST: return "RETROFE_ATTRACT_LAUNCH_REQUEST";
		case RETROFE_LAUNCH_ENTER: return "RETROFE_LAUNCH_ENTER";
		case RETROFE_LAUNCH_REQUEST: return "RETROFE_LAUNCH_REQUEST";
		case RETROFE_LAUNCH_EXIT: return "RETROFE_LAUNCH_EXIT";
		case RETROFE_BACK_REQUEST: return "RETROFE_BACK_REQUEST";
		case RETROFE_BACK_MENU_EXIT: return "RETROFE_BACK_MENU_EXIT";
		case RETROFE_BACK_MENU_LOAD_ART: return "RETROFE_BACK_MENU_LOAD_ART";
		case RETROFE_BACK_MENU_ENTER: return "RETROFE_BACK_MENU_ENTER";
		case RETROFE_MENUMODE_START_REQUEST: return "RETROFE_MENUMODE_START_REQUEST";
		case RETROFE_MENUMODE_START_LOAD_ART: return "RETROFE_MENUMODE_START_LOAD_ART";
		case RETROFE_MENUMODE_START_ENTER: return "RETROFE_MENUMODE_START_ENTER";
		case RETROFE_QUICKLIST_REQUEST: return "RETROFE_QUICKLIST_REQUEST";
		case RETROFE_QUICKLIST_PAGE_REQUEST: return "RETROFE_QUICKLIST_PAGE_REQUEST";
		case RETROFE_QUICKLIST_PAGE_MENU_EXIT: return "RETROFE_QUICKLIST_PAGE_MENU_EXIT";
		case RETROFE_SETTINGS_REQUEST: return "RETROFE_SETTINGS_REQUEST";
		case RETROFE_SETTINGS_PAGE_REQUEST: return "RETROFE_SETTINGS_PAGE_REQUEST";
		case RETROFE_SETTINGS_PAGE_MENU_EXIT: return "RETROFE_SETTINGS_PAGE_MENU_EXIT";
		case RETROFE_GAMEINFO_EXIT: return "RETROFE_GAMEINFO_EXIT";
		case RETROFE_GAMEINFO_ENTER: return "RETROFE_GAMEINFO_ENTER";
		case RETROFE_COLLECTIONINFO_ENTER: return "RETROFE_COLLECTIONINFO_ENTER";
		case RETROFE_COLLECIONINFO_EXIT: return "RETROFE_COLLECIONINFO_EXIT";
		case RETROFE_BUILDINFO_ENTER: return "RETROFE_BUILDINFO_ENTER";
		case RETROFE_BUILDINFO_EXIT: return "RETROFE_BUILDINFO_EXIT";
		case RETROFE_SCROLL_FORWARD: return "RETROFE_SCROLL_FORWARD";
		case RETROFE_SCROLL_BACK: return "RETROFE_SCROLL_BACK";
		case RETROFE_NEW: return "RETROFE_NEW";
		case RETROFE_QUIT_REQUEST: return "RETROFE_QUIT_REQUEST";
		case RETROFE_QUIT: return "RETROFE_QUIT";
		case RETROFE_SCROLL_PLAYLIST_FORWARD: return "RETROFE_SCROLL_PLAYLIST_FORWARD";
		case RETROFE_SCROLL_PLAYLIST_BACK: return "RETROFE_SCROLL_PLAYLIST_BACK";
		default: return "UNKNOWN_STATE_" + std::to_string(s);
	}
}

bool RetroFE::getAttractModeCyclePlaylist() {
	bool attractModeCyclePlaylist = true;
	std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
	std::string firstCollection = "";
	std::string cycleString = "";
	config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
	config_.getProperty(OPTION_ATTRACTMODECYCLEPLAYLIST, attractModeCyclePlaylist);
	config_.getProperty(OPTION_CYCLEPLAYLIST, cycleString);
	// use the global setting as overide if firstCollection == current
	if (cycleString == "" || firstCollection != currentPage_->getCollectionName())
	{
		// check if collection has different setting
		if (config_.propertyExists(settingPrefix + OPTION_ATTRACTMODECYCLEPLAYLIST))
		{
			config_.getProperty(settingPrefix + OPTION_ATTRACTMODECYCLEPLAYLIST, attractModeCyclePlaylist);
		}
	}

	return attractModeCyclePlaylist;
}

std::vector<std::string> RetroFE::getPlaylistCycle() {
	if (cycleVector_.empty())
	{
		std::string collectionName = currentPage_->getCollectionName();
		std::string settingPrefix = "collections." + collectionName + ".";

		std::string firstCollection = "";
		std::string cycleString = "";
		config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
		config_.getProperty(OPTION_CYCLEPLAYLIST, cycleString);
		// use the global setting as overide if firstCollection == current
		if (cycleString == "" || firstCollection != collectionName)
		{
			// check if collection has different setting
			if (config_.propertyExists(settingPrefix + OPTION_CYCLEPLAYLIST))
			{
				config_.getProperty(settingPrefix + OPTION_CYCLEPLAYLIST, cycleString);
			}
		}
		Utils::listToVector(cycleString, cycleVector_, ',');
	}

	return cycleVector_;
}

// Check if we can go back a page or quite RetroFE
bool RetroFE::back(bool& exit) {
	bool canGoBack = false;
	bool exitOnBack = false;
	config_.getProperty(OPTION_EXITONFIRSTPAGEBACK, exitOnBack);
	exit = false;

	if (currentPage_->getMenuDepth() <= 1 && pages_.empty())
	{
		exit = exitOnBack;
	}
	else
	{
		canGoBack = true;
	}

	return canGoBack;
}

// depricated - use kiosk mode
bool RetroFE::isStandalonePlaylist(std::string playlist) {
	// return playlist == "street fighter and capcom fighters" || playlist == "street fighter";
	return false;
}

bool RetroFE::isInAttractModeSkipPlaylist(std::string playlist) {
	if (lkupAttractModeSkipPlaylist_.empty())
	{
		std::string attractModeSkipPlaylist = "";
		std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
		std::string firstCollection = "";
		config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
		config_.getProperty(OPTION_ATTRACTMODESKIPPLAYLIST, attractModeSkipPlaylist);
		// use the global setting as overide if firstCollection == current
		if (attractModeSkipPlaylist == "" || firstCollection != currentPage_->getCollectionName())
		{
			// check if collection has different setting
			if (config_.propertyExists(settingPrefix + OPTION_ATTRACTMODESKIPPLAYLIST))
			{
				config_.getProperty(settingPrefix + OPTION_ATTRACTMODESKIPPLAYLIST, attractModeSkipPlaylist);
			}
		}

		if (attractModeSkipPlaylist != "")
		{
			// see if any of the comma seperated match current playlist
			std::stringstream ss(attractModeSkipPlaylist);
			std::string playlist = "";
			while (ss.good())
			{
				getline(ss, playlist, ',');
				lkupAttractModeSkipPlaylist_.try_emplace(playlist, true);
			}
		}
	}

	return !lkupAttractModeSkipPlaylist_.empty() &&
		lkupAttractModeSkipPlaylist_.find(playlist) != lkupAttractModeSkipPlaylist_.end();
}

void RetroFE::goToNextAttractModePlaylistByCycle(std::vector<std::string> cycleVector) {
	// find current position
	auto it = cycleVector.begin();
	while (it != cycleVector.end() && *it != currentPage_->getPlaylistName())
		++it;
	// find next playlist that is not in list
	for (;;)
	{
		if (!isInAttractModeSkipPlaylist(*it))
		{
			break;
		}
		++it;
		if (it == cycleVector.end())
			it = cycleVector.begin();
	}
	if (currentPage_->playlistExists(*it))
	{
		currentPage_->selectPlaylist(*it);
	}
}

// Add this function implementation to RetroFE.cpp
void RetroFE::handleMusicControls(UserInput::KeyCode_E input) {
	if (!musicPlayer_) {
		return;
	}
	switch (input)
	{
		case UserInput::KeyCodeMusicPlayPause:
		if (musicPlayer_->isPlaying())
		{
			musicPlayer_->pauseMusic();
		}
		else if (musicPlayer_->isPaused())
		{
			musicPlayer_->resumeMusic();
		}
		else
		{
			musicPlayer_->playMusic();
		}
		// Reset attract mode
		attract_.reset();
		break;

		case UserInput::KeyCodeMusicNext:
		musicPlayer_->nextTrack();
		// Reset attract mode
		attract_.reset();
		break;

		case UserInput::KeyCodeMusicPrev:
		musicPlayer_->previousTrack();
		// Reset attract mode
		attract_.reset();
		break;

		case UserInput::KeyCodeMusicVolumeUp:
		{
			musicPlayer_->changeVolume(true);
			// Reset attract mode
			attract_.reset();
		}
		break;

		case UserInput::KeyCodeMusicVolumeDown:
		{
			musicPlayer_->changeVolume(false);
			// Reset attract mode
			attract_.reset();
		}
		break;

		case UserInput::KeyCodeMusicToggleShuffle:
		musicPlayer_->setShuffle(!musicPlayer_->getShuffle());
		break;

		case UserInput::KeyCodeMusicToggleLoop:
		musicPlayer_->setLoop(!musicPlayer_->getLoop());
		break;

		default:
		break; // Do nothing for other key codes
	}
}

// Process the user input
RetroFE::RETROFE_STATE RetroFE::processUserInput(Page* page) {
	bool screensaver = false;
	config_.getProperty(OPTION_SCREENSAVER, screensaver);

	bool infoExitOnScroll = false;
	config_.getProperty(OPTION_INFOEXITONSCROLL, infoExitOnScroll);

	std::map<unsigned int, bool> ssExitInputs = {
		{SDL_MOUSEMOTION, true},          {SDL_KEYDOWN, true},
		{SDL_MOUSEBUTTONDOWN, true},      {SDL_JOYBUTTONDOWN, true},
		{SDL_JOYAXISMOTION, true},        {SDL_JOYHATMOTION, true},
		{SDL_CONTROLLERBUTTONDOWN, true}, {SDL_CONTROLLERAXISMOTION, true},
	};
	bool exit = false;
	RETROFE_STATE state = RETROFE_IDLE;

	// Poll all events until we find an active one
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		// some how !SDL_KEYUP prevents double action
		input_.update(e);
		if (e.type == SDL_POLLSENTINEL || (screensaver && ssExitInputs[e.type]))
		{
			break;
		}
	}



	if (screensaver && ssExitInputs[e.type])
	{
#ifdef WIN32
		Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 51, 0);
#endif
		return RETROFE_QUIT;
	}

	// Handle next/previous game inputs
	if (page->isHorizontalScroll())
	{
		// playlist scroll
		if (!kioskLock_ && input_.keystate(UserInput::KeyCodeDown)) {
			if (page->isGamesScrolling()) {
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_PLAYLIST_FORWARD;
		}
		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeUp)) {
			if (page->isGamesScrolling()) {
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_PLAYLIST_BACK;
		}

		// game scroll
		if (input_.keystate(UserInput::KeyCodeRight))
		{
			if (page->isPlaylistScrolling())
			{
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_FORWARD;
		}
		else if (input_.keystate(UserInput::KeyCodeLeft))
		{
			if (page->isPlaylistScrolling())
			{
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_BACK;
		}
	}
	else
	{
		// vertical
		//
		// playlist scroll
		if (!kioskLock_ && input_.keystate(UserInput::KeyCodeRight)) {
			if (page->isGamesScrolling()) {
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_PLAYLIST_FORWARD;
		}
		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeLeft)) {
			if (page->isGamesScrolling()) {
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_PLAYLIST_BACK;
		}

		// game scroll
		if (input_.keystate(UserInput::KeyCodeDown))
		{
			if (page->isPlaylistScrolling())
			{
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_FORWARD;
		}
		else if (input_.keystate(UserInput::KeyCodeUp))
		{
			if (page->isPlaylistScrolling())
			{
				return RETROFE_HIGHLIGHT_REQUEST;
			}
			attract_.reset();
			if (infoExitOnScroll)
			{
				resetInfoToggle();
			}
			return RETROFE_SCROLL_BACK;
		}
	}

	if (input_.keystate(UserInput::KeyCodeMusicVolumeUp))
	{
		keyLastTime_ = currentTime_;
		handleMusicControls(UserInput::KeyCodeMusicVolumeUp);
		return state;
	}
	else if (input_.keystate(UserInput::KeyCodeMusicVolumeDown))
	{
		keyLastTime_ = currentTime_;
		handleMusicControls(UserInput::KeyCodeMusicVolumeDown);
		return state;
	}

	// don't wait for idle
	if (currentTime_ - keyLastTime_ > keyDelayTime_)
	{
		if (input_.keystate(UserInput::KeyCodeMusicPlayPause))
		{
			keyLastTime_ = currentTime_;
			handleMusicControls(UserInput::KeyCodeMusicPlayPause);
			return state;
		}
		else if (input_.keystate(UserInput::KeyCodeMusicNext))
		{
			keyLastTime_ = currentTime_;
			handleMusicControls(UserInput::KeyCodeMusicNext);
			return state;
		}
		else if (input_.keystate(UserInput::KeyCodeMusicPrev))
		{
			keyLastTime_ = currentTime_;
			handleMusicControls(UserInput::KeyCodeMusicPrev);
			return state;
		}
		else if (input_.keystate(UserInput::KeyCodeMusicToggleShuffle))
		{
			keyLastTime_ = currentTime_;
			handleMusicControls(UserInput::KeyCodeMusicToggleShuffle);
			return state;
		}
		else if (input_.keystate(UserInput::KeyCodeMusicToggleLoop))
		{
			keyLastTime_ = currentTime_;
			handleMusicControls(UserInput::KeyCodeMusicToggleLoop);
			return state;
		}

		// lock or unlock playlist/collection/menu nav and fav toggle
		if (page->isIdle() && input_.keystate(UserInput::KeyCodeKisok))
		{
			attract_.reset();
			kioskLock_ = !kioskLock_;
			page->setLocked(kioskLock_);
			page->onNewItemSelected();

			keyLastTime_ = currentTime_;
			return RETROFE_IDLE;
		}
		else if (input_.keystate(UserInput::KeyCodeShowFps)) {
			keyLastTime_ = currentTime_;
			if (!debugFont_) {
				std::string fontPath = Configuration::absolutePath + "/font.ttf";
				debugFont_ = TTF_OpenFont(fontPath.c_str(), 24);
				if (!debugFont_)
				{
					LOG_ERROR("RetroFE", "Could not load font: " + fontPath);
					return state;
				}
				else
				{
					LOG_INFO("RetroFE", "Loaded debug font: " + fontPath);
				}
			}
			showFps_ = !showFps_;
		}
		else if (input_.keystate(UserInput::KeyCodeMenu) && !menuMode_)
		{
			keyLastTime_ = currentTime_;
			return RETROFE_MENUMODE_START_REQUEST;
		}
		else if (input_.keystate(UserInput::KeyCodeSettingsCombo1) && input_.keystate(UserInput::KeyCodeSettingsCombo2))
		{
			attract_.reset();
			bool controllerComboSettings = false;
			config_.getProperty(OPTION_CONTROLLERCOMBOSETTINGS, controllerComboSettings);
			if (controllerComboSettings)
			{
				return RETROFE_SETTINGS_REQUEST;
			}
		}
		else if (input_.keystate(UserInput::KeyCodeQuitCombo1) && input_.keystate(UserInput::KeyCodeQuitCombo2))
		{
			attract_.reset();
			bool controllerComboExit = false;
			config_.getProperty(OPTION_CONTROLLERCOMBOEXIT, controllerComboExit);
			if (controllerComboExit)
			{
#ifdef WIN32
				Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 51, 0);
#endif
				return RETROFE_QUIT_REQUEST;
			}
		}
		// KeyCodeCycleCollection shared with KeyCodeQuitCombo1 and can missfire
		else if (!kioskLock_ && input_.lastKeyPressed(UserInput::KeyCodeCycleCollection))
		{
			// delay a bit longer for next cycle or ignore second keyboard hit count
			if (!(currentTime_ - keyLastTime_ > keyDelayTime_ + 1.0))
			{
				return RETROFE_IDLE;
			}
			input_.resetStates();
			keyLastTime_ = currentTime_;
			resetInfoToggle();
			attract_.reset();
			if (collectionCycle_.size())
			{
				collectionCycleIt_++;
				if (collectionCycleIt_ == collectionCycle_.end())
				{
					collectionCycleIt_ = collectionCycle_.begin();
				}
				if (!pages_.empty() && pages_.size() > 1)
					pages_.pop();

				if (nextPageItem_) {
					delete nextPageItem_;
				}
				nextPageItem_ = new Item();
				nextPageItem_->name = *collectionCycleIt_;
				menuMode_ = false;

				return RETROFE_NEXT_PAGE_REQUEST;
			}
			return RETROFE_IDLE;
		}
		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodePrevCycleCollection))
		{
			// delay a bit longer for next cycle or ignore second keyboard hit count
			if (!(currentTime_ - keyLastTime_ > keyDelayTime_ + 1.0))
			{
				return RETROFE_IDLE;
			}
			input_.resetStates();
			keyLastTime_ = currentTime_;
			resetInfoToggle();
			attract_.reset();
			if (collectionCycle_.size())
			{
				if (collectionCycleIt_ == collectionCycle_.begin())
				{
					collectionCycleIt_ = collectionCycle_.end();
				}
				collectionCycleIt_--;

				if (!pages_.empty() && pages_.size() > 1)
					pages_.pop();

				if (nextPageItem_) {
					delete nextPageItem_;
				}
				nextPageItem_ = new Item();
				nextPageItem_->name = *collectionCycleIt_;
				menuMode_ = false;

				return RETROFE_NEXT_PAGE_REQUEST;
			}
			return RETROFE_IDLE;
		}
		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeQuickList))
		{
			attract_.reset();
			state = RETROFE_QUICKLIST_REQUEST;
		}
		else if (!kioskLock_ && (input_.keystate(UserInput::KeyCodeCyclePlaylist) ||
			input_.keystate(UserInput::KeyCodeNextCyclePlaylist)))
		{
			if (!isStandalonePlaylist(currentPage_->getPlaylistName()))
			{
				resetInfoToggle();
				attract_.reset();
				keyLastTime_ = currentTime_;
				return RETROFE_PLAYLIST_NEXT_CYCLE;
			}
		}
		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodePrevCyclePlaylist))
		{
			if (!isStandalonePlaylist(currentPage_->getPlaylistName()))
			{
				resetInfoToggle();
				attract_.reset();
				keyLastTime_ = currentTime_;
				return RETROFE_PLAYLIST_PREV_CYCLE;
			}
		}
		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeBack))
		{
			resetInfoToggle();
			attract_.reset();
			if (back(exit) || exit)
			{
				// if collection cycle then also update it's position
				if (collectionCycle_.size())
				{
					if (collectionCycleIt_ != collectionCycle_.begin())
					{
						collectionCycleIt_--;
					}
				}
				keyLastTime_ = currentTime_;
				return exit ? RETROFE_QUIT_REQUEST : RETROFE_BACK_REQUEST;
			}
		}
	}

	// Ignore other keys while the menu is scrolling
	if (page->isIdle() && currentTime_ - keyLastTime_ > keyDelayTime_)
	{
		// Handle Collection Up/Down keys
		if (!kioskLock_ && ((input_.keystate(UserInput::KeyCodeCollectionUp) &&
			(page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeUp))) ||
			(input_.keystate(UserInput::KeyCodeCollectionLeft) &&
				(!page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeLeft)))))
		{
			resetInfoToggle();
			attract_.reset();
			bool backOnCollection = false;
			config_.getProperty(OPTION_BACKONCOLLECTION, backOnCollection);
			if (page->getMenuDepth() == 1 || !backOnCollection)
				state = RETROFE_COLLECTION_UP_REQUEST;
			else
				state = RETROFE_BACK_REQUEST;
		}

		else if (!kioskLock_ && ((input_.keystate(UserInput::KeyCodeCollectionDown) &&
			(page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeDown))) ||
			(input_.keystate(UserInput::KeyCodeCollectionRight) &&
				(!page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeRight)))))
		{
			resetInfoToggle();
			attract_.reset();
			bool backOnCollection = false;
			config_.getProperty(OPTION_BACKONCOLLECTION, backOnCollection);
			if (page->getMenuDepth() == 1 || !backOnCollection)
				state = RETROFE_COLLECTION_DOWN_REQUEST;
			else
				state = RETROFE_BACK_REQUEST;
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodePageUp))
		{
			resetInfoToggle();
			attract_.reset();
			page->pageScroll(Page::ScrollDirectionBack);
			state = RETROFE_MENUJUMP_REQUEST;
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodePageDown))
		{
			resetInfoToggle();
			attract_.reset();
			page->pageScroll(Page::ScrollDirectionForward);
			state = RETROFE_MENUJUMP_REQUEST;
		}

		else if (input_.keystate(UserInput::KeyCodeLetterUp))
		{
			resetInfoToggle();
			attract_.reset();
			if (currentPage_->getPlaylistName() != "lastplayed")
			{
				// if playlist same name as meta sort value then support meta jump
				if (Item::validSortType(page->getPlaylistName()))
				{
					page->metaScroll(Page::ScrollDirectionBack, page->getPlaylistName());
				}
				else
				{
					bool cfwLetterSub;
					config_.getProperty(OPTION_CFWLETTERSUB, cfwLetterSub);
					if (cfwLetterSub && page->hasSubs())
						page->cfwLetterSubScroll(Page::ScrollDirectionBack);
					else
						page->letterScroll(Page::ScrollDirectionBack);
				}
				state = RETROFE_MENUJUMP_REQUEST;
			}
		}

		else if (input_.keystate(UserInput::KeyCodeLetterDown))
		{
			resetInfoToggle();
			attract_.reset();
			if (currentPage_->getPlaylistName() != "lastplayed")
			{
				if (Item::validSortType(page->getPlaylistName()))
				{
					page->metaScroll(Page::ScrollDirectionForward, page->getPlaylistName());
				}
				else
				{
					bool cfwLetterSub;
					config_.getProperty(OPTION_CFWLETTERSUB, cfwLetterSub);
					if (cfwLetterSub && page->hasSubs())
						page->cfwLetterSubScroll(Page::ScrollDirectionForward);
					else
						page->letterScroll(Page::ScrollDirectionForward);
				}
				state = RETROFE_MENUJUMP_REQUEST;
			}
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeFavPlaylist))
		{
			attract_.reset();
			page->favPlaylist();
			state = RETROFE_PLAYLIST_REQUEST;
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeSettings))
		{
			attract_.reset();
			state = RETROFE_SETTINGS_REQUEST;
		}

		else if (!kioskLock_ && (input_.keystate(UserInput::KeyCodeNextPlaylist) ||
			(input_.keystate(UserInput::KeyCodePlaylistDown) && page->isHorizontalScroll()) ||
			(input_.keystate(UserInput::KeyCodePlaylistRight) && !page->isHorizontalScroll())))
		{
			resetInfoToggle();
			attract_.reset();
			state = RETROFE_PLAYLIST_NEXT;
		}

		else if (!kioskLock_ && (input_.keystate(UserInput::KeyCodePrevPlaylist) ||
			(input_.keystate(UserInput::KeyCodePlaylistUp) && page->isHorizontalScroll()) ||
			(input_.keystate(UserInput::KeyCodePlaylistLeft) && !page->isHorizontalScroll())))
		{
			resetInfoToggle();
			attract_.reset();
			state = RETROFE_PLAYLIST_PREV;
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeRemovePlaylist))
		{
			attract_.reset();
			page->rememberSelectedItem();
			page->removePlaylist();

			// don't trigger playlist change events but refresh item states
			currentPage_->reallocateMenuSpritePoints(); // update playlist menu

			state = RETROFE_PLAYLIST_ENTER;
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeAddPlaylist))
		{
			if (!isStandalonePlaylist(currentPage_->getPlaylistName()))
			{
				attract_.reset();
				page->rememberSelectedItem();
				page->addPlaylist();
				// don't trigger playlist change events but refresh item states
				currentPage_->onNewItemSelected();
				state = RETROFE_PLAYLIST_ENTER;
			}
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeTogglePlaylist))
		{
			if (currentPage_->getPlaylistName() != "favorites" &&
				!isStandalonePlaylist(currentPage_->getPlaylistName()))
			{
				attract_.reset();
				page->rememberSelectedItem();
				page->togglePlaylist();
				// don't trigger playlist change events but refresh item states
				currentPage_->onNewItemSelected();
				state = RETROFE_PLAYLIST_ENTER;
			}
		}

		else if (input_.keystate(UserInput::KeyCodeToggleGameInfo) || (input_.keystate(UserInput::KeyCodeGameInfoCombo1) && input_.keystate(UserInput::KeyCodeGameInfoCombo2)))
		{
			attract_.reset();
			input_.resetStates();
			keyLastTime_ = currentTime_;
			if (collectionInfo_)
			{
				currentPage_->collectionInfoExit();
				collectionInfo_ = false;
			}
			else if (buildInfo_)
			{
				currentPage_->buildInfoExit();
				buildInfo_ = false;
			}
			state = RETROFE_GAMEINFO_ENTER;
			if (gameInfo_)
			{
				state = RETROFE_GAMEINFO_EXIT;
			}
			gameInfo_ = !gameInfo_;
		}

		else if (input_.keystate(UserInput::KeyCodeToggleCollectionInfo) || (input_.keystate(UserInput::KeyCodeCollectionInfoCombo1) && input_.keystate(UserInput::KeyCodeCollectionInfoCombo2)))
		{
			attract_.reset();
			input_.resetStates();
			keyLastTime_ = currentTime_;
			if (gameInfo_)
			{
				currentPage_->gameInfoExit();
				gameInfo_ = false;
			}
			else if (buildInfo_)
			{
				currentPage_->buildInfoExit();
				buildInfo_ = false;
			}
			state = RETROFE_COLLECTIONINFO_ENTER;
			if (collectionInfo_)
			{
				state = RETROFE_COLLECIONINFO_EXIT;
			}
			collectionInfo_ = !collectionInfo_;
		}
		else if (input_.keystate(UserInput::KeyCodeToggleBuildInfo) || (input_.keystate(UserInput::KeyCodeBuildInfoCombo1) && input_.keystate(UserInput::KeyCodeBuildInfoCombo2)))
		{
			attract_.reset();
			input_.resetStates();
			keyLastTime_ = currentTime_;
			if (gameInfo_)
			{
				currentPage_->gameInfoExit();
				gameInfo_ = false;
			}
			else if (collectionInfo_)
			{
				currentPage_->collectionInfoExit();
				collectionInfo_ = false;
			}
			state = RETROFE_BUILDINFO_ENTER;
			if (buildInfo_)
			{
				state = RETROFE_BUILDINFO_EXIT;
			}
			buildInfo_ = !buildInfo_;
		}

		else if (input_.keystate(UserInput::KeyCodeSkipForward))
		{
			attract_.reset();
			page->skipForward();
			page->jukeboxJump();
			keyLastTime_ = currentTime_;
		}

		else if (input_.keystate(UserInput::KeyCodeSkipBackward))
		{
			attract_.reset();
			page->skipBackward();
			page->jukeboxJump();
			keyLastTime_ = currentTime_;
		}

		else if (input_.keystate(UserInput::KeyCodeSkipForwardp))
		{
			attract_.reset();
			page->skipForwardp();
			page->jukeboxJump();
			keyLastTime_ = currentTime_;
		}

		else if (input_.keystate(UserInput::KeyCodeSkipBackwardp))
		{
			attract_.reset();
			page->skipBackwardp();
			page->jukeboxJump();
			keyLastTime_ = currentTime_;
		}

		else if (input_.keystate(UserInput::KeyCodePause))
		{
			page->pause();
			page->jukeboxJump();
			keyLastTime_ = currentTime_;
			paused_ = !paused_;
			if (!paused_)
			{
				// trigger attract on unpause
				attract_.activate();
			}
		}

		else if (input_.keystate(UserInput::KeyCodeRestart))
		{
			attract_.reset();
			page->restart();
			keyLastTime_ = currentTime_;
		}

		else if (input_.keystate(UserInput::KeyCodeRandom))
		{
			attract_.reset();
			page->selectRandom();
			state = RETROFE_MENUJUMP_REQUEST;
		}

		else if (input_.keystate(UserInput::KeyCodeAdminMode))
		{
			// todo: add admin mode support
		}

		else if (input_.keystate(UserInput::KeyCodeSelect) && !currentPage_->isMenuScrolling())
		{
			resetInfoToggle();
			attract_.reset();
			nextPageItem_ = page->getSelectedItem();
			if (nextPageItem_)
			{
				if (nextPageItem_->leaf)
				{
					if (menuMode_)
					{
						state = RETROFE_HANDLE_MENUENTRY;
					}
					else
					{
						state = RETROFE_LAUNCH_ENTER;
					}
				}
				else
				{
					CollectionInfoBuilder cib(config_, *metadb_);
					std::string lastPlayedSkipCollection = "";
					int size = 0;
					config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
					config_.getProperty(OPTION_LASTPLAYEDSIZE, size);

					if (!isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()) &&
						nextPageItem_->collectionInfo->name != lastPlayedSkipCollection)
					{
						cib.updateLastPlayedPlaylist(
							currentPage_->getCollection(), nextPageItem_,
							size); // Update last played playlist if not currently in the skip playlist (e.g. settings)
						currentPage_->updateReloadables(0);
					}
					state = RETROFE_NEXT_PAGE_REQUEST;
				}
			}
		}

		// !kioskLock_ &&
		else if (input_.keystate(UserInput::KeyCodeQuit))
		{
			attract_.reset();
#ifdef WIN32
			Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 51, 0);
#endif
			state = RETROFE_QUIT_REQUEST;
		}

		// !kioskLock_ &&
		else if (input_.keystate(UserInput::KeyCodeReboot))
		{
			attract_.reset();
			reboot_ = true;
			state = RETROFE_QUIT_REQUEST;
		}

		else if (!kioskLock_ && input_.keystate(UserInput::KeyCodeSaveFirstPlaylist))
		{
			resetInfoToggle();
			attract_.reset();
			if (page->getMenuDepth() == 1)
			{
				firstPlaylist_ = page->getPlaylistName();
				saveRetroFEState();
			}
		}
	}


	if (state != RETROFE_IDLE)
	{
		keyLastTime_ = currentTime_;
		return state;
	}

	// Check if we're done scrolling
	if (!input_.keystate(UserInput::KeyCodeUp) && !input_.keystate(UserInput::KeyCodeLeft) &&
		!input_.keystate(UserInput::KeyCodeDown) && !input_.keystate(UserInput::KeyCodeRight) &&
		!input_.keystate(UserInput::KeyCodePlaylistUp) && !input_.keystate(UserInput::KeyCodePlaylistLeft) &&
		!input_.keystate(UserInput::KeyCodePlaylistDown) && !input_.keystate(UserInput::KeyCodePlaylistRight) &&
		!input_.keystate(UserInput::KeyCodeCollectionUp) && !input_.keystate(UserInput::KeyCodeCollectionLeft) &&
		!input_.keystate(UserInput::KeyCodeCollectionDown) && !input_.keystate(UserInput::KeyCodeCollectionRight) &&
		!input_.keystate(UserInput::KeyCodePageUp) && !input_.keystate(UserInput::KeyCodePageDown) &&
		!input_.keystate(UserInput::KeyCodeLetterUp) && !input_.keystate(UserInput::KeyCodeLetterDown) &&
		!attract_.isActive())
	{
		page->resetScrollPeriod();
		if (page->isMenuScrolling())
		{
			attract_.reset(attract_.isSet());
			state = RETROFE_HIGHLIGHT_REQUEST;
		}
	}

	return state;
}

// Load a page
Page* RetroFE::loadPage(const std::string& collectionName) {
	// check if collection's assets are in a different theme
	std::string layoutName;
	config_.getProperty("collections." + collectionName + ".layout", layoutName);
	if (layoutName == "")
	{
		config_.getProperty(OPTION_LAYOUT, layoutName);
	}
	PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);
	Page* page = pb.buildPage(collectionName);
	if (!page)
	{
		LOG_ERROR("RetroFE", "Could not create page");
	}
	else
	{
		if (page->controlsType() != "")
		{
			updatePageControls(page->controlsType());
		}
	}

	return page;
}

// Load the splash page
Page* RetroFE::loadSplashPage() {
	std::string layoutName;
	config_.getProperty(OPTION_LAYOUT, layoutName);

	PageBuilder pb(layoutName, "splash", config_, &fontcache_);
	Page* page = pb.buildPage();
	if (!page)
	{
		LOG_ERROR("RetroFE", "Could not create splash page");
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Configuration Error",
			"RetroFE is unable to create a splash page from the given splash.xml", NULL);
	}
	else
	{
		page->start();
	}
	return page;
}

// Load a collection
CollectionInfo* RetroFE::getCollection(const std::string& collectionName) {

	// Check if subcollections should be merged or split
	bool subsSplit = false;
	config_.getProperty(OPTION_SUBSSPLIT, subsSplit);

	// Build the collection
	CollectionInfoBuilder cib(config_, *metadb_);
	CollectionInfo* collection = cib.buildCollection(collectionName);
	collection->subsSplit = subsSplit;
	cib.injectMetadata(collection);

	// Check collection folder exists
	fs::path path = Utils::combinePath(Configuration::absolutePath, "collections", collectionName);
	if (!fs::exists(path) || !fs::is_directory(path))
	{
		LOG_ERROR("RetroFE", "Failed to load collection " + collectionName);
		return nullptr;
	}

	// Loading sub collection files
	for (const auto& entry : fs::directory_iterator(path))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".sub")
		{
			std::string basename = entry.path().stem().string();

			LOG_INFO("RetroFE", "Loading subcollection into menu: " + basename);

			CollectionInfo* subcollection = cib.buildCollection(basename, collectionName);
			collection->addSubcollection(subcollection);
			subcollection->subsSplit = subsSplit;
			cib.injectMetadata(subcollection);
			collection->hasSubs = true;
		}
	}

	// sort a collection's items
	bool menuSort = true;
	config_.getProperty("collections." + collectionName + ".list.menuSort", menuSort);
	if (menuSort)
	{
		config_.getProperty("collections." + collectionName + ".list.sortType", collection->sortType);
		if (!Item::validSortType(collection->sortType))
		{
			collection->sortType = "";
		}
		collection->sortItems();
	}

	MenuParser mp;
	bool menuFromCollectionLaunchers = false;
	config_.getProperty("collections." + collectionName + ".menuFromCollectionLaunchers", menuFromCollectionLaunchers);
	if (menuFromCollectionLaunchers)
	{
		// build menu out of all found collections that have launcherfiles
		std::string collectionLaunchers = "collectionLaunchers";
		std::string launchers = "";
		config_.getProperty(collectionLaunchers, launchers);
		if (launchers != "")
		{
			std::vector<std::string> launcherVector;
			std::stringstream ss(launchers);
			while (ss.good())
			{
				std::string substr;
				getline(ss, substr, ',');
				if (substr != "")
				{
					launcherVector.push_back(substr);
				}
			}
			mp.buildMenuFromCollectionLaunchers(collection, launcherVector);
		}
		else
		{
			// todo log error
		}
	}
	else
	{
		// build collection menu if menu.txt exists
		mp.buildMenuItems(collection, menuSort);
	}

	// adds items to "all" list except those found in "exclude_all.txt"
	cib.addPlaylists(collection);
	collection->sortPlaylists();

	// Add extra info, if available
	std::string defaultPath =
		Utils::combinePath(Configuration::absolutePath, "collections", collectionName, "info", "default.conf");
	for (auto& item : collection->items)
	{
		item->loadInfo(defaultPath);
		std::string path = Utils::combinePath(Configuration::absolutePath, "collections", collectionName, "info",
			item->name + ".conf");
		item->loadInfo(path);
	}

	// Remove parenthesis and brackets, if so configured
	bool showParenthesis = true;
	bool showSquareBrackets = true;

	(void)config_.getProperty(OPTION_SHOWPARENTHESIS, showParenthesis);
	(void)config_.getProperty(OPTION_SHOWSQUAREBRACKETS, showSquareBrackets);

	// using Playlists_T = std::map<std::string, std::vector<Item *> *>;
	for (auto const& playlistPair : collection->playlists)
	{
		for (auto& item : *(playlistPair.second))
		{
			if (!showParenthesis)
			{
				std::string::size_type firstPos = item->title.find_first_of("(");
				std::string::size_type secondPos = item->title.find_first_of(")", firstPos);

				while (firstPos != std::string::npos && secondPos != std::string::npos)
				{
					firstPos = item->title.find_first_of("(");
					secondPos = item->title.find_first_of(")", firstPos);

					if (firstPos != std::string::npos)
					{
						item->title.erase(firstPos, (secondPos - firstPos) + 1);
					}
				}
			}
			if (!showSquareBrackets)
			{
				std::string::size_type firstPos = item->title.find_first_of("[");
				std::string::size_type secondPos = item->title.find_first_of("]", firstPos);

				while (firstPos != std::string::npos && secondPos != std::string::npos)
				{
					firstPos = item->title.find_first_of("[");
					secondPos = item->title.find_first_of("]", firstPos);

					if (firstPos != std::string::npos && secondPos != std::string::npos)
					{
						item->title.erase(firstPos, (secondPos - firstPos) + 1);
					}
				}
			}
		}
	}

	return collection;
}

void RetroFE::updatePageControls(const std::string& type) {
	LOG_INFO("Layout", "Layout changed controls type " + type);
	std::string controlsConfPath = Utils::combinePath(Configuration::absolutePath, "controls");
	if (config_.import("controls", controlsConfPath + " - " + type + ".conf"))
	{
		input_.reconfigure();
	}
}

// Load a menu
CollectionInfo* RetroFE::getMenuCollection(const std::string& collectionName) {
	std::string menuPath = Utils::combinePath(Configuration::absolutePath, "menu");
	std::string menuFile = Utils::combinePath(menuPath, collectionName + ".txt");
	std::vector<Item*> menuVector;
	CollectionInfoBuilder cib(config_, *metadb_);
	auto* collection = new CollectionInfo(config_, collectionName, menuPath, "", "", "");
	cib.ImportBasicList(collection, menuFile, menuVector);

	for (auto& item : menuVector)
	{
		item->leaf = false;
		if (size_t position = item->name.find("="); position != std::string::npos)
		{
			item->ctrlType = Utils::trimEnds(item->name.substr(position + 1));
			item->name = Utils::trimEnds(item->name.substr(0, position));
			item->title = item->name;
			item->fullTitle = item->name;
			item->leaf = true;
		}
		item->collectionInfo = collection;
		collection->items.push_back(item);
	}
	collection->playlists["all"] = &collection->items;
	return collection;
}

bool RetroFE::isUserActive(double now, double threshold) const {
	return (now - keyLastTime_) < threshold;
}

void RetroFE::saveRetroFEState() const {
	std::string file = Utils::combinePath(Configuration::absolutePath, "settings_saved.conf");
	LOG_INFO("RetroFE", "Saving settings_saved.conf");
	std::ofstream filestream;
	try
	{
		filestream.open(file.c_str());
		filestream << "firstPlaylist = " << firstPlaylist_ << std::endl;
		filestream.close();
	}
	catch (std::exception&)
	{
		LOG_ERROR("RetroFE", "Save failed: " + file);
	}
}

std::string RetroFE::getLayoutFileName() {
	std::string layoutName = "layout";
	std::string randomLayoutNames;
	config_.getProperty(OPTION_RANDOMLAYOUT, randomLayoutNames);
	if (randomLayoutNames != "")
	{
		LOG_INFO("RetroFE", "Choosing random layout from: " + randomLayoutNames);
		std::vector<std::string> randomLayoutVector;
		Utils::listToVector(randomLayoutNames, randomLayoutVector, ',');
		if (randomLayoutVector.size() > 1)
		{
			layoutName = randomLayoutVector[rand() % randomLayoutVector.size()];
		}
		else
		{
			layoutName = randomLayoutVector[0];
		}
	}

	return layoutName;
}

void RetroFE::resetInfoToggle() {
	if (gameInfo_)
	{
		currentPage_->gameInfoExit();
		gameInfo_ = false;
	}
	else if (collectionInfo_)
	{
		currentPage_->collectionInfoExit();
		collectionInfo_ = false;
	}
	else if (buildInfo_)
	{
		currentPage_->buildInfoExit();
		buildInfo_ = false;
	}
}

MetadataDatabase* RetroFE::getMetaDb() {
	return metadb_;
}
