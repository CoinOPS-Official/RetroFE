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


#include "SDL.h"
#include "Database/Configuration.h"
#include "Database/GlobalOpts.h"
#include "Utility/Log.h"
#include "Sound/AudioBus.h"
#include "Sound/MusicPlayer.h"
#if __has_include(<SDL_mixer.h>)
#include <SDL_mixer.h>
#elif __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif
#include "Utility/Utils.h"
#include <algorithm>
#include <cctype>
#include <cmath>

std::vector<SDL_Window*>    SDL::window_;
std::vector<SDL_Renderer*>  SDL::renderer_;
static std::vector<SDL_Texture*> renderTargets_;
static std::vector<int> renderTargetWidths_;
static std::vector<int> renderTargetHeights_;
std::vector<int>            SDL::displayWidth_;
std::vector<int>            SDL::displayHeight_;
std::vector<int>            SDL::windowWidth_;
std::vector<int>            SDL::windowHeight_;
std::vector<int>            SDL::displayRefresh_;
std::vector<bool>           SDL::fullscreen_;
std::vector<int>            SDL::rotation_;
std::vector<bool>           SDL::mirror_;
int                         SDL::numScreens_ = 1;
int                         SDL::numDisplays_ = 1;
int                         SDL::screenCount_;
SDL::LayoutScaleMode SDL::layoutScaleMode_ = SDL::LayoutScaleMode::Stretch;

// Initialize SDL
bool SDL::initialize(Configuration& config) {
	int audioRate = 48000;
	Uint16 audioFormat = MIX_DEFAULT_FORMAT; // 16-bit stereo
	int audioChannels = 2;
	int audioBuffers = 4096;
	bool hideMouse;

	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
	SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

#ifdef WIN32
	if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
	{
		LOG_ERROR("SDL", "Unable to set DPI awareness hint");
	}
#endif
	if (SDL_WasInit(0) == 0) {
		// First-time startup: Initialize everything.
		LOG_INFO("SDL", "Performing first-time full initialization of all SDL subsystems.");
		if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
		{
			std::string error = SDL_GetError();
			LOG_ERROR("SDL", "Initial SDL_Init failed: " + error);
			return false;
		}
	}
	else {
		// Re-initialization: Audio and input are already running. Only initialize video.
		// --- THIS IS THE ROBUST RETRY LOGIC FOR THE PI5 RACE CONDITION ---
		LOG_INFO("SDL", "Attempting to re-initialize video subsystem...");
		const int MAX_RETRIES = 10;
		const int RETRY_DELAY_MS = 100;
		bool success = false;
		for (int i = 0; i < MAX_RETRIES; ++i) {
			if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
				success = true;
				LOG_INFO("SDL", "Video subsystem re-initialized successfully on attempt " + std::to_string(i + 1) + ".");
				break;
			}
			LOG_WARNING("SDL", "Failed to re-initialize video subsystem (attempt " + std::to_string(i + 1) + "/" + std::to_string(MAX_RETRIES) + "): " + std::string(SDL_GetError()) + ". Retrying...");
			SDL_Delay(RETRY_DELAY_MS);
		}
		if (!success) {
			LOG_ERROR("SDL", "Failed to re-initialize video subsystem after " + std::to_string(MAX_RETRIES) + " attempts. Giving up.");
			return false;
		}
	}

#ifdef WIN32
	std::string SDLRenderDriver = "direct3d11";
	config.getProperty(OPTION_SDLRENDERDRIVER, SDLRenderDriver);
	if (SDL_SetHint(SDL_HINT_RENDER_DRIVER, SDLRenderDriver.c_str()) != SDL_TRUE)
	{
		LOG_ERROR("SDL", "Error setting renderer to " + SDLRenderDriver + ". Available: direct3d, direct3d11, direct3d12, opengl, opengles2, opengles, metal, and software");
	}
#endif

	std::string ScaleQuality = "1";
	config.getProperty(OPTION_SCALEQUALITY, ScaleQuality);
	if (SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, ScaleQuality.c_str()) != SDL_TRUE)
	{
		LOG_ERROR("SDL", "Failed to set scale quality hint to " + ScaleQuality);
	}

	SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1"); // For all renderers

	std::string layoutScaleModeString = "stretch";
	config.getProperty(OPTION_LAYOUTSCALEMODE, layoutScaleModeString);

	std::transform(
		layoutScaleModeString.begin(),
		layoutScaleModeString.end(),
		layoutScaleModeString.begin(),
		[](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		}
	);

	if (layoutScaleModeString == "fit") {
		layoutScaleMode_ = LayoutScaleMode::Fit;
	}
	else if (layoutScaleModeString == "fill") {
		layoutScaleMode_ = LayoutScaleMode::Fill;
	}
	else {
		if (layoutScaleModeString != "stretch") {
			LOG_WARNING(
				"SDL",
				"Invalid layoutScaleMode '" + layoutScaleModeString +
				"'. Valid values are stretch, fit, and fill. "
				"Defaulting to stretch."
			);
		}

		layoutScaleMode_ = LayoutScaleMode::Stretch;
	}

	LOG_INFO(
		"SDL",
		"Layout scale mode: " +
		std::string(
			layoutScaleMode_ == LayoutScaleMode::Fill ? "fill" :
			layoutScaleMode_ == LayoutScaleMode::Fit ? "fit" :
			"stretch"
		)
	);

	if (config.getProperty(OPTION_HIDEMOUSE, hideMouse))
		SDL_ShowCursor(hideMouse ? SDL_FALSE : SDL_TRUE);

	// --- Configuration for hardware/video/audio ---
	bool HardwareVideoAccel = false;
	config.getProperty(OPTION_HARDWAREVIDEOACCEL, HardwareVideoAccel);
	Configuration::HardwareVideoAccel = HardwareVideoAccel;
	int AvdecMaxThreads = 2;
	config.getProperty(OPTION_AVDECMAXTHREADS, AvdecMaxThreads);
	Configuration::AvdecMaxThreads = AvdecMaxThreads;
	int AvdecThreadType = 2;
	config.getProperty(OPTION_AVDECTHREADTYPE, AvdecThreadType);
	Configuration::AvdecThreadType = AvdecThreadType;
	bool MuteVideo = false;
	config.getProperty(OPTION_MUTEVIDEO, MuteVideo);
	Configuration::MuteVideo = MuteVideo;

	// --- Parse screenOrder (with backwards compatibility) ---
	std::string screenOrderStr;
	if (config.propertyExists(OPTION_SCREENORDER)) {
		config.getProperty(OPTION_SCREENORDER, screenOrderStr);
		LOG_INFO("SDL", "Using configured screenOrder: " + screenOrderStr);
	}
	else {
		// Fallback: use legacy screenNumX or numScreens
		int numScreens = -1;
		config.getProperty("numScreens", numScreens);

		if (numScreens > 0) {
			for (int i = 0; i < numScreens; ++i) {
				int screenNum = i;
				config.getProperty("screenNum" + std::to_string(i), screenNum);
				if (!screenOrderStr.empty()) screenOrderStr += ",";
				screenOrderStr += std::to_string(screenNum);
			}
			LOG_INFO("SDL", "No screenOrder specified. Using screenNumX and numScreens: " + screenOrderStr);
		}
		else {
			// Auto-detect as fallback
			std::vector<int> legacyScreenNums;
			for (int i = 0;; ++i) {
				std::string key = "screenNum" + std::to_string(i);
				int val;
				if (config.getProperty(key, val)) {
					legacyScreenNums.push_back(val);
				}
				else {
					break;
				}
			}
			if (!legacyScreenNums.empty()) {
				for (size_t i = 0; i < legacyScreenNums.size(); ++i) {
					if (i > 0) screenOrderStr += ",";
					screenOrderStr += std::to_string(legacyScreenNums[i]);
				}
				LOG_INFO("SDL", "No screenOrder or numScreens specified. Using detected screenNumX: " + screenOrderStr);
			}
			else {
				screenOrderStr = "0";
				LOG_WARNING("SDL", "No screenOrder, screenNumX, or numScreens specified. Defaulting to screen 0.");
			}
		}
	}
	// Split and convert to vector<int>
	std::vector<std::string> screenOrderStrVec;
	Utils::listToVector(screenOrderStr, screenOrderStrVec, ',');

	std::vector<int> screenOrder;
	for (const auto& s : screenOrderStrVec) {
		try {
			int idx = std::stoi(s);
			screenOrder.push_back(idx);
		}
		catch (...) {
			LOG_WARNING("SDL", "Invalid entry in screenOrder: '" + s + "' (not an integer). Ignored.");
		}
	}

	int numDisplays = SDL_GetNumVideoDisplays();
	if (numDisplays < 1) {
		LOG_ERROR("SDL", "No SDL video displays detected.");
		return false;
	}

	// --- Validate and filter screenOrder entries ---
	std::vector<int> validScreenOrder;
	for (auto displayIndex : screenOrder) {
		if (displayIndex < numDisplays && displayIndex >= 0) {
			validScreenOrder.push_back(displayIndex);
		}
		else {
			LOG_WARNING("SDL", "screenOrder entry " + std::to_string(displayIndex) +
				" ignored (only " + std::to_string(numDisplays) + " displays present).");
		}
	}
	if (validScreenOrder.empty()) {
		LOG_ERROR("SDL", "No valid displays listed in screenOrder! Initialization aborted.");
		return false;
	}

	screenOrder = validScreenOrder;
	screenCount_ = static_cast<int>(screenOrder.size());
	LOG_INFO("SDL", "Number of displays found: " + std::to_string(numDisplays));
	LOG_INFO("SDL", "Number of screens requested: " + std::to_string(screenCount_));

	// --- OPENGL SYSTEM RAM OPTIMIZATIONS ---
	// Force Core Profile to strip legacy OpenGL state and save RAM
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

	// Disable depth and stencil buffers to prevent VRAM/System RAM backing allocation
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
	// ---------------------------------------

	// --- Per-screen initialization loop ---
	for (int logicalScreen = 0; logicalScreen < screenCount_; ++logicalScreen)
	{
		int physicalDisplay = screenOrder[logicalScreen];
		SDL_DisplayMode mode;
		bool windowBorder = false;
		bool windowResize = false;
		Uint32 windowFlags = SDL_WINDOW_OPENGL;
		std::string screenIndex = std::to_string(logicalScreen);
		config.getProperty(OPTION_WINDOWBORDER, windowBorder);
		if (!windowBorder)
			windowFlags |= SDL_WINDOW_BORDERLESS;
		config.getProperty(OPTION_WINDOWRESIZE, windowResize);
		if (windowResize)
			windowFlags |= SDL_WINDOW_RESIZABLE;

		if (SDL_GetCurrentDisplayMode(physicalDisplay, &mode) != 0)
		{
			if (logicalScreen == 0)
			{
				LOG_ERROR("SDL", "Display " + std::to_string(physicalDisplay) + " does not exist.");
				return false;
			}
			else
			{
				LOG_WARNING("SDL", "Display " + std::to_string(physicalDisplay) + " does not exist.");
				windowWidth_.push_back(0);
				windowHeight_.push_back(0);
				displayWidth_.push_back(0);
				displayHeight_.push_back(0);
				window_.push_back(NULL);
				renderer_.push_back(NULL);
				continue;
			}
		}

		displayRefresh_.push_back(mode.refresh_rate);
		windowWidth_.push_back(mode.w);
		displayWidth_.push_back(mode.w);
		std::string hString = "";
		if (logicalScreen == 0)
			config.getProperty(OPTION_HORIZONTAL, hString);
		config.getProperty(OPTION_HORIZONTAL + screenIndex, hString);
		if (hString == "")
		{
			LOG_ERROR("Configuration", "Missing property \"horizontal\"" + screenIndex);
			return false;
		}
		else if (hString == "envvar")
		{
			hString = Utils::getEnvVar("H_RES_" + screenIndex);
			if (hString == "" || !Utils::convertInt(hString))
			{
				LOG_WARNING("Configuration", "Invalid property value for \"horizontal\"" + screenIndex + " defaulted to 'stretch'");
			}
			else
			{
				LOG_WARNING("Configuration", "H_RES_" + screenIndex + " for  \"horizontal\" set to " + hString);
				windowWidth_[logicalScreen] = Utils::convertInt(hString);
			}
		}
		else if (hString != "stretch" &&
			(logicalScreen != 0 || !config.getProperty(OPTION_HORIZONTAL, windowWidth_[logicalScreen])) &&
			!config.getProperty(OPTION_HORIZONTAL + screenIndex, windowWidth_[logicalScreen]))
		{
			LOG_ERROR("Configuration", "Invalid property value for \"horizontal\"" + screenIndex);
			return false;
		}

		windowHeight_.push_back(mode.h);
		displayHeight_.push_back(mode.h);
		std::string vString = "";
		if (logicalScreen == 0)
			config.getProperty(OPTION_VERTICAL, vString);
		config.getProperty(OPTION_VERTICAL + screenIndex, vString);
		if (vString == "")
		{
			LOG_ERROR("Configuration", "Missing property \"vertical\"" + screenIndex);
			return false;
		}
		else if (vString == "envvar")
		{
			vString = Utils::getEnvVar("V_RES_" + screenIndex);
			if (vString == "" || !Utils::convertInt(vString))
			{
				LOG_WARNING("Configuration", "Invalid property value for \"vertical\"" + screenIndex + " defaulted to 'stretch'");
			}
			else
			{
				LOG_WARNING("Configuration", "V_RES_" + screenIndex + " for  \"vertical\" set to " + vString);
				windowHeight_[logicalScreen] = Utils::convertInt(vString);
			}
		}
		else if (vString != "stretch" &&
			(logicalScreen != 0 || !config.getProperty(OPTION_VERTICAL, windowHeight_[logicalScreen])) &&
			!config.getProperty(OPTION_VERTICAL + screenIndex, windowHeight_[logicalScreen]))
		{
			LOG_ERROR("Configuration", "Invalid property value for \"vertical\"" + screenIndex);
			return false;
		}

		bool fullscreen = false;
		config.getProperty(OPTION_FULLSCREEN, fullscreen);
		if (logicalScreen == 0 && !config.getProperty(OPTION_FULLSCREEN, fullscreen) && !config.getProperty(OPTION_FULLSCREEN + screenIndex, fullscreen))
		{
			LOG_ERROR("Configuration", "Missing property: \"fullscreen\"" + screenIndex);
			return false;
		}
		fullscreen_.push_back(fullscreen);

		if (fullscreen_[logicalScreen])
		{
#ifdef WIN32
			windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#elif defined(__APPLE__)
			windowFlags |= SDL_WINDOW_BORDERLESS;
#else
			windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif
		}
		else
		{
#ifdef WIN32
			// No additional flags needed for borderless fullscreen on Windows
#else
			windowFlags |= SDL_WINDOW_BORDERLESS;
#endif
		}

		int rotation = 0;
		config.getProperty(OPTION_ROTATION + screenIndex, rotation);
		LOG_INFO("Configuration", "Setting rotation for screen " + screenIndex + " to " + std::to_string(rotation * 90) + " degrees.");
		rotation_.push_back(rotation);

		bool mirror = false;
		config.getProperty(OPTION_MIRROR + screenIndex, mirror);
		if (mirror)
			LOG_INFO("Configuration", "Setting mirror mode for screen " + screenIndex + ".");
		mirror_.push_back(mirror);

		window_.push_back(NULL);
		renderer_.push_back(NULL);
		std::string fullscreenStr = fullscreen_[logicalScreen] ? "yes" : "no";
		std::stringstream ss;
		ss << "Creating " << windowWidth_[logicalScreen] << "x" << windowHeight_[logicalScreen]
			<< " window (fullscreen: " << fullscreenStr << ")"
			<< " for logical screen " << logicalScreen
			<< " on physical display " << physicalDisplay;
		LOG_INFO("SDL", ss.str());
		std::string retrofeTitle = "RetroFE " + std::to_string(physicalDisplay);
		if (!window_[logicalScreen])
		{
			window_[logicalScreen] = SDL_CreateWindow(retrofeTitle.c_str(),
				SDL_WINDOWPOS_CENTERED_DISPLAY(physicalDisplay), SDL_WINDOWPOS_CENTERED_DISPLAY(physicalDisplay),
				windowWidth_[logicalScreen], windowHeight_[logicalScreen], windowFlags);
		}

		if (window_[logicalScreen] == NULL)
		{
			std::string error = SDL_GetError();
			if (logicalScreen == 0)
			{
				LOG_ERROR("SDL", "Create window " + screenIndex + " on display " + std::to_string(physicalDisplay) + " failed: " + error);
				return false;
			}
			else
			{
				LOG_WARNING("SDL", "Create window " + screenIndex + " on display " + std::to_string(physicalDisplay) + " failed: " + error);
			}
		}
		else
		{
			if (logicalScreen == 0)
			{
#ifndef __APPLE__
				SDL_WarpMouseInWindow(window_[logicalScreen], windowWidth_[logicalScreen], 0);
#else
				SDL_WarpMouseInWindow(window_[logicalScreen], windowWidth_[logicalScreen] / 2, windowHeight_[logicalScreen] / 2);
#endif
				SDL_SetRelativeMouseMode(SDL_TRUE);
			}
			bool vSync = false;
			config.getProperty(OPTION_VSYNC, vSync);
			if (!renderer_[logicalScreen])
			{
				if (vSync)
				{
					renderer_[logicalScreen] = SDL_CreateRenderer(window_[logicalScreen], -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
					LOG_INFO("SDL", "vSync Enabled");
				}
				else
				{
					renderer_[logicalScreen] = SDL_CreateRenderer(window_[logicalScreen], -1, SDL_RENDERER_ACCELERATED);
				}
			}
			if (renderer_[logicalScreen] == NULL)
			{
				std::string error = SDL_GetError();
				LOG_ERROR("SDL", "Create renderer " + screenIndex + " failed: " + error);
				return false;
			}
			else
			{
				// Logical page targets are created lazily once the page reports
				// its per-monitor layout dimensions.
				renderTargets_.resize(screenCount_, nullptr);
				renderTargetWidths_.resize(screenCount_, 0);
				renderTargetHeights_.resize(screenCount_, 0);

				SDL_RendererInfo info;
				if (SDL_GetRendererInfo(renderer_[logicalScreen], &info) == 0)
				{
					std::string screenIndexStr = std::to_string(logicalScreen);
					std::string logMessage = "Current rendering backend for renderer " + screenIndexStr + ": ";
					logMessage += info.name;
					LOG_INFO("SDL", logMessage);

					// Log the supported pixel formats
					logMessage = "Supported pixel formats for renderer " + screenIndexStr + ":";
					for (Uint32 i = 0; i < info.num_texture_formats; ++i)
					{
						const char* formatName = SDL_GetPixelFormatName(info.texture_formats[i]);
						logMessage += "\n  - " + std::string(formatName);
					}
					LOG_INFO("SDL", logMessage);

					if (strcmp(info.name, "opengl") == 0)
					{
						int GlSwapInterval = 1;
						config.getProperty(OPTION_GLSWAPINTERVAL, GlSwapInterval);
						if (SDL_GL_SetSwapInterval(GlSwapInterval) < 0)
						{
							LOG_ERROR("SDL", "Unable to set OpenGL swap interval: " + std::string(SDL_GetError()));
						}
					}
				}
				else
				{
					LOG_ERROR("SDL", "Could not retrieve renderer info for renderer " + screenIndex + " Error: " + SDL_GetError());
				}
			}
		}
	}

	bool minimizeOnFocusLoss;
	if (config.getProperty(OPTION_MINIMIZEONFOCUSLOSS, minimizeOnFocusLoss))
	{
		SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, minimizeOnFocusLoss ? "1" : "0", SDL_HINT_OVERRIDE);
	}

	int num_audio_devices_open = Mix_QuerySpec(nullptr, nullptr, nullptr);

	if (num_audio_devices_open == 0) {
		// No audio device is open, so initialize it and the decoders.
		if (Mix_OpenAudio(audioRate, audioFormat, audioChannels, audioBuffers) == -1)
		{
			std::string error = Mix_GetError();
			LOG_WARNING("SDL", "Audio initialize failed: " + error);
		}
		else
		{
			// If we successfully opened the audio device, IMMEDIATELY initialize the decoders.
			int flags = MIX_INIT_MP3 | MIX_INIT_OGG;
			int initialized_flags = Mix_Init(flags);
			if ((initialized_flags & flags) != flags) {
				LOG_ERROR("SDL", "Mix_Init failed to initialize all requested decoders: " + std::string(Mix_GetError()));
			}
			else {
				LOG_INFO("SDL", "SDL_mixer decoders (MP3, OGG, etc.) initialized successfully.");
			}
			// --- NEW: configure AudioBus to match the device SDL_mixer opened ---
			AudioBus::instance().configureFromMixer();
			// Define a tiny context
			struct PostMixCtx {
				MusicPlayer* mp;   // nullptr if music player disabled
			};

			// ... during init ...
			bool musicPlayerEnabled = false;
			config.getProperty("musicPlayer.enabled", musicPlayerEnabled);

			// If enabled, ensure the instance exists *before* installing the callback
			MusicPlayer* mp = nullptr;
			if (musicPlayerEnabled) {
				mp = MusicPlayer::getInstance();
			}

			// Context must outlive the audio device; make it static or allocate it
			static PostMixCtx g_postmix_ctx{ mp };

			Mix_SetPostMix(
				[](void* udata, Uint8* stream, int len) {
					auto* ctx = static_cast<PostMixCtx*>(udata);

					// 1) MUSIC-ONLY visualization: notify visualizers if enabled
					if (ctx && ctx->mp) {
						ctx->mp->processAudioData(stream, len);
					}

					// 2) Mix in external (GStreamer) audio AFTER visualizers saw music-only
					AudioBus::instance().mixInto(stream, len);

					// 3) optional: master metering on final mix goes here
				},
				&g_postmix_ctx
			);
		}
	}

	return true;
}

// Deinitialize SDL
bool SDL::deInitialize(bool fullShutdown) { // The 'fullShutdown' parameter is key
	LOG_INFO("SDL", "DeInitializing");

	// Step 1: Always destroy windows and renderers, as they are part of the video subsystem.
	if (!window_.empty() && window_[0])
	{
#ifdef __APPLE__
		SDL_SetRelativeMouseMode(SDL_FALSE);
#endif
		SDL_WarpMouseInWindow(window_[0], windowWidth_[0] / 2, windowHeight_[0] / 2);
	}
	else
	{
		LOG_WARNING("SDL", "Window 0 is NULL, cannot center mouse within it");
	}

// Destroy render target textures
	for (auto& t : renderTargets_) {
		if (t) {
			SDL_DestroyTexture(t);
			t = nullptr;
		}
	}
	renderTargets_.clear();
	renderTargetWidths_.clear();
	renderTargetHeights_.clear();

	// Destroy renderers and windows
	for (auto renderer : renderer_)
	{
		if (renderer) SDL_DestroyRenderer(renderer);
	}
	renderer_.clear();

	for (auto window : window_)
	{
		if (window) SDL_DestroyWindow(window);
	}
	window_.clear();

	// Step 2: Decide which subsystems to shut down.
	if (fullShutdown)
	{
		SDL_ShowCursor(SDL_TRUE);
		// This is the final application exit. Shut down everything.
		LOG_INFO("SDL", "Performing full de-initialization of all SDL subsystems.");
		Mix_CloseAudio();
		Mix_Quit();
		SDL_Quit();

	}
	else
	{
		// This is the unloadSDL case. Shut down ONLY the video subsystem.
		LOG_INFO("SDL", "De-initializing video subsystem only.");
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}

	displayWidth_.clear();
	displayHeight_.clear();
	windowWidth_.clear();
	windowHeight_.clear();
	fullscreen_.clear();
	mirror_.clear();
	rotation_.clear();

	return true;
}


// Get the renderer
SDL_Renderer* SDL::getRenderer(int index) {
	if (renderer_.empty()) {
		return nullptr;
	}
	return (index < screenCount_ ? renderer_[index] : renderer_[0]);
}

std::string SDL::getRendererBackend(int index) {
	SDL_Renderer* renderer = getRenderer(index);
	if (!renderer) {
		return "Invalid renderer index";
	}

	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(renderer, &info) != 0) {
		return std::string("Error getting renderer info: ") + SDL_GetError();
	}

	return std::string(info.name);
}

// Get the window
SDL_Window* SDL::getWindow(int index) {
	if (window_.empty()) {
		return nullptr;
	}
	return (index < screenCount_ ? window_[index] : window_[0]);
}

// current target to render into for this frame
SDL_Texture* SDL::getRenderTarget(int index) {
	if (renderTargets_.empty()) return nullptr;
	return (index < screenCount_ ? renderTargets_[index] : renderTargets_[0]);
}

bool SDL::ensureRenderTarget(
	int index,
	int layoutWidth,
	int layoutHeight) {

	if (index < 0 ||
		index >= screenCount_ ||
		!renderer_[index] ||
		layoutWidth <= 0 ||
		layoutHeight <= 0)
	{
		return false;
	}

	if (renderTargets_.size() < static_cast<std::size_t>(screenCount_)) {
		renderTargets_.resize(screenCount_, nullptr);
		renderTargetWidths_.resize(screenCount_, 0);
		renderTargetHeights_.resize(screenCount_, 0);
	}

	if (renderTargets_[index] &&
		renderTargetWidths_[index] == layoutWidth &&
		renderTargetHeights_[index] == layoutHeight)
	{
		return true;
	}

	SDL_Renderer* renderer = renderer_[index];

	SDL_Texture* replacement =
		SDL_CreateTexture(
			renderer,
			SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_TARGET,
			layoutWidth,
			layoutHeight
		);

	if (!replacement) {
		LOG_ERROR(
			"SDL",
			"Failed to create " +
			std::to_string(layoutWidth) + "x" +
			std::to_string(layoutHeight) +
			" logical render target for screen " +
			std::to_string(index) + ": " +
			std::string(SDL_GetError())
		);
		return false;
	}

	SDL_SetTextureBlendMode(replacement, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(replacement, SDL_ScaleModeLinear);

	SDL_Texture* previous = renderTargets_[index];
	renderTargets_[index] = replacement;
	renderTargetWidths_[index] = layoutWidth;
	renderTargetHeights_[index] = layoutHeight;

	if (previous) {
		if (SDL_GetRenderTarget(renderer) == previous) {
			SDL_SetRenderTarget(renderer, nullptr);
		}
		SDL_DestroyTexture(previous);
	}

	return true;
}

bool SDL::presentRenderTarget(
	int index,
	int layoutWidth,
	int layoutHeight) {

	if (index < 0 ||
		index >= screenCount_ ||
		!renderer_[index] ||
		layoutWidth <= 0 ||
		layoutHeight <= 0 ||
		renderTargets_.size() <= static_cast<std::size_t>(index) ||
		renderTargetWidths_.size() <= static_cast<std::size_t>(index) ||
		renderTargetHeights_.size() <= static_cast<std::size_t>(index) ||
		!renderTargets_[index] ||
		renderTargetWidths_[index] != layoutWidth ||
		renderTargetHeights_[index] != layoutHeight)
	{
		return false;
	}

	SDL_Renderer* renderer = renderer_[index];
	SDL_Texture* target = renderTargets_[index];

	int outputWidth = 0;
	int outputHeight = 0;

	if (SDL_GetRendererOutputSize(
		renderer,
		&outputWidth,
		&outputHeight) != 0)
	{
		LOG_ERROR(
			"SDL",
			"SDL_GetRendererOutputSize failed for screen " +
			std::to_string(index) + ": " +
			std::string(SDL_GetError())
		);
		return false;
	}

	if (outputWidth <= 0 || outputHeight <= 0) {
		return true;
	}

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

	if (SDL_RenderClear(renderer) != 0) {
		LOG_ERROR(
			"SDL",
			"Failed to clear screen " +
			std::to_string(index) +
			" before presentation: " +
			std::string(SDL_GetError())
		);
		return false;
	}

	const int rotation = rotation_[index] & 3;

	/*
	 * Preserve the historical fullscreen compensation as a single
	 * presentation translation. It used to be added to every logical
	 * element before output rotation.
	 */
	float fullscreenOffsetX = 0.0f;
	float fullscreenOffsetY = 0.0f;

	if (fullscreen_[index] &&
		windowWidth_[index] > 0 &&
		windowHeight_[index] > 0)
	{
		const float nominalOffsetX =
			0.5f * static_cast<float>(
				displayWidth_[index] - windowWidth_[index]
				);

		const float nominalOffsetY =
			0.5f * static_cast<float>(
				displayHeight_[index] - windowHeight_[index]
				);

		float rotatedOffsetX = nominalOffsetX;
		float rotatedOffsetY = nominalOffsetY;

		/*
		 * Mirror presentation historically distinguishes only even from
		 * odd rotations: 0/2 share one arrangement and 1/3 share the
		 * other. Use that same effective rotation for its translation.
		 */
		const int offsetRotation =
			mirror_[index]
			? (rotation & 1)
			: rotation;

		switch (offsetRotation) {
			case 1:
				rotatedOffsetX = -nominalOffsetY;
				rotatedOffsetY = nominalOffsetX;
				break;

			case 2:
				rotatedOffsetX = -nominalOffsetX;
				rotatedOffsetY = -nominalOffsetY;
				break;

			case 3:
				rotatedOffsetX = nominalOffsetY;
				rotatedOffsetY = -nominalOffsetX;
				break;

			default:
				break;
		}

		fullscreenOffsetX =
			rotatedOffsetX *
			static_cast<float>(outputWidth) /
			static_cast<float>(windowWidth_[index]);

		fullscreenOffsetY =
			rotatedOffsetY *
			static_cast<float>(outputHeight) /
			static_cast<float>(windowHeight_[index]);
	}

	auto copyTarget =
		[&](const SDL_FRect& destination, double angle) -> bool
		{
			return SDL_RenderCopyExF(
				renderer,
				target,
				nullptr,
				&destination,
				angle,
				nullptr,
				SDL_FLIP_NONE
			) == 0;
		};

	bool result = true;

	if (mirror_[index]) {
		/*
		 * Cabinet mirror mode historically ignores layoutScaleMode and
		 * stretches one logical scene across half of the output. Present
		 * the completed scene twice instead of duplicating every element.
		 */
		SDL_FRect first{};

		if ((rotation & 1) == 0) {
			first = {
				fullscreenOffsetX,
				static_cast<float>(outputHeight) * 0.5f +
					fullscreenOffsetY,
				static_cast<float>(outputWidth),
				static_cast<float>(outputHeight) * 0.5f
			};

			result &= copyTarget(first, 0.0);

			SDL_FRect second{
				static_cast<float>(outputWidth) -
					first.x - first.w,
				static_cast<float>(outputHeight) -
					first.y - first.h,
				first.w,
				first.h
			};

			result &= copyTarget(second, 180.0);
		}
		else {
			first = {
				static_cast<float>(outputWidth) * 0.25f -
					static_cast<float>(outputHeight) * 0.5f +
					fullscreenOffsetX,
				static_cast<float>(outputHeight) * 0.5f -
					static_cast<float>(outputWidth) * 0.25f +
					fullscreenOffsetY,
				static_cast<float>(outputHeight),
				static_cast<float>(outputWidth) * 0.5f
			};

			result &= copyTarget(first, 90.0);

			SDL_FRect second{
				static_cast<float>(outputWidth) -
					first.x - first.w,
				static_cast<float>(outputHeight) -
					first.y - first.h,
				first.w,
				first.h
			};

			result &= copyTarget(second, 270.0);
		}
	}
	else {
		const float availableWidth =
			(rotation & 1)
			? static_cast<float>(outputHeight)
			: static_cast<float>(outputWidth);

		const float availableHeight =
			(rotation & 1)
			? static_cast<float>(outputWidth)
			: static_cast<float>(outputHeight);

		const float candidateScaleX =
			availableWidth / static_cast<float>(layoutWidth);

		const float candidateScaleY =
			availableHeight / static_cast<float>(layoutHeight);

		SDL_FRect destination{};

		switch (layoutScaleMode_) {
			case LayoutScaleMode::Fit: {
				const float scale =
					std::min(candidateScaleX, candidateScaleY);

				destination.w =
					static_cast<float>(layoutWidth) * scale;

				destination.h =
					static_cast<float>(layoutHeight) * scale;
				break;
			}

			case LayoutScaleMode::Fill: {
				const float scale =
					std::max(candidateScaleX, candidateScaleY);

				destination.w =
					static_cast<float>(layoutWidth) * scale;

				destination.h =
					static_cast<float>(layoutHeight) * scale;
				break;
			}

			case LayoutScaleMode::Stretch:
			default:
				destination.w = availableWidth;
				destination.h = availableHeight;
				break;
		}

		destination.x =
			0.5f *
			(static_cast<float>(outputWidth) - destination.w) +
			fullscreenOffsetX;

		destination.y =
			0.5f *
			(static_cast<float>(outputHeight) - destination.h) +
			fullscreenOffsetY;

		result &= copyTarget(
			destination,
			static_cast<double>(rotation * 90)
		);
	}

	if (!result) {
		LOG_ERROR(
			"SDL",
			"Failed to present logical render target for screen " +
			std::to_string(index) + ": " +
			std::string(SDL_GetError())
		);
	}

	return result;
}

// Render a copy of a texture
bool SDL::renderCopy(SDL_Texture* texture, float alpha, SDL_Rect const* src, SDL_Rect const* dest, ViewInfo& viewInfo, int layoutWidth, int layoutHeight) {

	if (!dest) {
		return false;
	}

	const SDL_FRect destination{
		static_cast<float>(dest->x),
		static_cast<float>(dest->y),
		static_cast<float>(dest->w),
		static_cast<float>(dest->h)
	};

	return renderCopyF(
		texture,
		alpha,
		src,
		&destination,
		viewInfo,
		layoutWidth,
		layoutHeight
	);

}

bool SDL::renderCopyF(SDL_Texture* texture,
	float alpha,
	const SDL_Rect* src,
	const SDL_FRect* dest,
	ViewInfo& viewInfo,
	int layoutWidth,
	int layoutHeight) {

	if (!texture) {
		return false;
	}

	if (alpha <= 0.0f) {
		return true;
	}

	if (!dest || layoutWidth <= 0 || layoutHeight <= 0) {
		return false;
	}

	const int m = viewInfo.Monitor;

	if (m < 0 || m >= screenCount_ || !renderer_[m]) {
		return true;
	}

	/*
	 * Components now render into a target whose dimensions are exactly the
	 * logical layout dimensions. Monitor-wide scaling, output rotation,
	 * fullscreen compensation, and cabinet mirror duplication are applied
	 * once by presentRenderTarget().
	 */
	const int outW = layoutWidth;
	const int outH = layoutHeight;

	if (outW <= 0 || outH <= 0) {
		return true;
	}

	/*
	 * Preserve the historical order of operations for locally rotated
	 * elements under non-uniform Stretch or cabinet scaling:
	 *
	 *     scale the element rectangle, then rotate it
	 *
	 * A final whole-scene stretch would normally reverse that order. The
	 * geometry path pre-distorts locally rotated quads in layout space so
	 * the final presentation stretch produces the same output geometry.
	 */
	float legacyElementScaleX = 1.0f;
	float legacyElementScaleY = 1.0f;

	const int presentationRotation = rotation_[m] & 3;

	const float nominalOutputWidth =
		(presentationRotation & 1)
		? static_cast<float>(windowHeight_[m])
		: static_cast<float>(windowWidth_[m]);

	const float nominalOutputHeight =
		(presentationRotation & 1)
		? static_cast<float>(windowWidth_[m])
		: static_cast<float>(windowHeight_[m]);

	if (mirror_[m]) {
		legacyElementScaleX =
			nominalOutputWidth /
			static_cast<float>(layoutWidth);

		legacyElementScaleY =
			0.5f *
			nominalOutputHeight /
			static_cast<float>(layoutHeight);
	}
	else if (layoutScaleMode_ == LayoutScaleMode::Stretch) {
		legacyElementScaleX =
			nominalOutputWidth /
			static_cast<float>(layoutWidth);

		legacyElementScaleY =
			nominalOutputHeight /
			static_cast<float>(layoutHeight);
	}

	legacyElementScaleX =
		std::max(legacyElementScaleX, 1e-6f);

	legacyElementScaleY =
		std::max(legacyElementScaleY, 1e-6f);

	// ---------------------------------------------------------
	// Texture and source/destination setup
	// ---------------------------------------------------------

	int texW = static_cast<int>(viewInfo.ImageWidth);
	int texH = static_cast<int>(viewInfo.ImageHeight);

	if (texW <= 0 || texH <= 0) {
		if (SDL_QueryTexture(
			texture,
			nullptr,
			nullptr,
			&texW,
			&texH) != 0)
		{
			LOG_ERROR(
				"SDL",
				"SDL_QueryTexture failed in renderCopyF: " +
				std::string(SDL_GetError())
			);
			return false;
		}

		viewInfo.ImageWidth = static_cast<float>(texW);
		viewInfo.ImageHeight = static_cast<float>(texH);
	}

	const float invTexW =
		1.0f / static_cast<float>(texW);

	const float invTexH =
		1.0f / static_cast<float>(texH);

	// Texture color modulation is invariant for the base and reflection
	// quads emitted by this renderCopyF() call.
	Uint8 textureR = 255;
	Uint8 textureG = 255;
	Uint8 textureB = 255;

	SDL_GetTextureColorMod(
		texture,
		&textureR,
		&textureG,
		&textureB
	);

	SDL_Rect srcRect =
		src ? *src : SDL_Rect{ 0, 0, texW, texH };

	SDL_FRect dstRect = *dest;

	if (dstRect.w <= 0.0f ||
		dstRect.h <= 0.0f ||
		srcRect.w <= 0 ||
		srcRect.h <= 0)
	{
		return true;
	}

	SDL_FRect container{};

	bool hasContainer =
		viewInfo.ContainerWidth > 0.0f &&
		viewInfo.ContainerHeight > 0.0f;

	if (mirror_[m] && !hasContainer) {
		container = {
			0.0f,
			0.0f,
			static_cast<float>(layoutWidth),
			static_cast<float>(layoutHeight)
		};

		hasContainer = true;
	}
	else if (hasContainer) {
		container = {
			viewInfo.ContainerX,
			viewInfo.ContainerY,
			viewInfo.ContainerWidth,
			viewInfo.ContainerHeight
		};
	}

	const SDL_Rect src0 = srcRect;
	const SDL_FRect dst0 = dstRect;

	// ---------------------------------------------------------
	// Helpers
	// ---------------------------------------------------------

	auto clamp_int = [](int value, int minimum, int maximum) {
		return value < minimum
			? minimum
			: value > maximum
			? maximum
			: value;
		};

	auto clamp_u8 = [&](float alpha01) -> Uint8 {
		alpha01 = std::clamp(alpha01, 0.0f, 1.0f);

		return static_cast<Uint8>(
			clamp_int(
				static_cast<int>(std::lround(alpha01 * 255.0f)),
				0,
				255
			)
			);
		};

	auto recompute_src_from_dst =
		[&](SDL_Rect& sourceRect,
			const SDL_Rect& sourceCopy,
			const SDL_FRect& destinationRect,
			const SDL_FRect& destinationCopy)
		{
			const float sourceScaleX =
				destinationCopy.w > 0.0f
				? static_cast<float>(sourceCopy.w) /
				destinationCopy.w
				: 0.0f;

			const float sourceScaleY =
				destinationCopy.h > 0.0f
				? static_cast<float>(sourceCopy.h) /
				destinationCopy.h
				: 0.0f;

			sourceRect.w = static_cast<int>(
				std::lround(destinationRect.w * sourceScaleX)
				);

			sourceRect.h = static_cast<int>(
				std::lround(destinationRect.h * sourceScaleY)
				);

			sourceRect.x =
				sourceCopy.x +
				static_cast<int>(
					std::lround(
						(destinationRect.x - destinationCopy.x) *
						sourceScaleX
					)
					);

			sourceRect.y =
				sourceCopy.y +
				static_cast<int>(
					std::lround(
						(destinationRect.y - destinationCopy.y) *
						sourceScaleY
					)
					);

			const SDL_Rect limit =
				src ? *src : SDL_Rect{ 0, 0, texW, texH };

			if (sourceRect.x < limit.x) {
				sourceRect.x = limit.x;
			}

			if (sourceRect.y < limit.y) {
				sourceRect.y = limit.y;
			}

			if (sourceRect.x + sourceRect.w >
				limit.x + limit.w)
			{
				sourceRect.w = std::max(
					0,
					limit.x + limit.w - sourceRect.x
				);
			}

			if (sourceRect.y + sourceRect.h >
				limit.y + limit.h)
			{
				sourceRect.h = std::max(
					0,
					limit.y + limit.h - sourceRect.y
				);
			}
		};

	/*
	 * Clip one axis-aligned logical destination rectangle to another and
	 * adjust the source rectangle so texture mapping remains unchanged.
	 *
	 * This stays entirely in layout space. Monitor-wide presentation is
	 * applied later to the completed target.
	 */
	auto clip_to_rect =
		[&](SDL_Rect& sourceRect,
			SDL_FRect& destinationRect,
			const SDL_FRect& clipRect)
		{
			if (destinationRect.w <= 0.0f ||
				destinationRect.h <= 0.0f ||
				clipRect.w <= 0.0f ||
				clipRect.h <= 0.0f)
			{
				destinationRect.w = 0.0f;
				destinationRect.h = 0.0f;
				sourceRect.w = 0;
				sourceRect.h = 0;
				return;
			}

			const float clipRight =
				clipRect.x + clipRect.w;

			const float clipBottom =
				clipRect.y + clipRect.h;

			const float destinationRight =
				destinationRect.x + destinationRect.w;

			const float destinationBottom =
				destinationRect.y + destinationRect.h;

			// Fully outside: reject without doing source-coordinate math.
			if (destinationRight <= clipRect.x ||
				destinationBottom <= clipRect.y ||
				destinationRect.x >= clipRight ||
				destinationRect.y >= clipBottom)
			{
				destinationRect.w = 0.0f;
				destinationRect.h = 0.0f;
				sourceRect.w = 0;
				sourceRect.h = 0;
				return;
			}

			// Common case: already fully contained, so clipping is a no-op.
			if (destinationRect.x >= clipRect.x &&
				destinationRect.y >= clipRect.y &&
				destinationRight <= clipRight &&
				destinationBottom <= clipBottom)
			{
				return;
			}

			const SDL_Rect sourceCopy = sourceRect;
			const SDL_FRect destinationCopy = destinationRect;

			const float clippedRight =
				std::min(destinationRight, clipRight);

			const float clippedBottom =
				std::min(destinationBottom, clipBottom);

			destinationRect.x =
				std::max(destinationCopy.x, clipRect.x);

			destinationRect.y =
				std::max(destinationCopy.y, clipRect.y);

			destinationRect.w =
				clippedRight - destinationRect.x;

			destinationRect.h =
				clippedBottom - destinationRect.y;

			recompute_src_from_dst(
				sourceRect,
				sourceCopy,
				destinationRect,
				destinationCopy
			);
		};

	auto draw_quad =
		[&](const SDL_Rect& sourceRect,
			const SDL_FRect& destinationPixels,
			float angleDegrees,
			bool flipHorizontal,
			bool flipVertical,
			float alpha01) -> bool
		{
			constexpr float epsilon = 1.0f;

			SDL_FPoint points[4];

			/*
			 * Fast path for the overwhelmingly common unrotated case.
			 *
			 * This avoids fmod(), trig/table selection, rotated AABB math,
			 * center calculations, and the four-point rotation loop.
			 */
			if (angleDegrees == 0.0f) {
				if (destinationPixels.x + destinationPixels.w < -epsilon ||
					destinationPixels.y + destinationPixels.h < -epsilon ||
					destinationPixels.x >
					static_cast<float>(outW) + epsilon ||
					destinationPixels.y >
					static_cast<float>(outH) + epsilon)
				{
					return true;
				}

				points[0] = {
					destinationPixels.x,
					destinationPixels.y
				};

				points[1] = {
					destinationPixels.x + destinationPixels.w,
					destinationPixels.y
				};

				points[2] = {
					destinationPixels.x + destinationPixels.w,
					destinationPixels.y + destinationPixels.h
				};

				points[3] = {
					destinationPixels.x,
					destinationPixels.y + destinationPixels.h
				};
			}
			else {
				float cosAngle;
				float sinAngle;

				const float remainder =
					std::fmod(std::fabs(angleDegrees), 90.0f);

				if (remainder < 0.001f ||
					remainder > 89.999f)
				{
					int quarter =
						static_cast<int>(
							std::lround(angleDegrees / 90.0f)
							) % 4;

					if (quarter < 0) {
						quarter += 4;
					}

					static constexpr float cosTable[] = {
						1.0f, 0.0f, -1.0f, 0.0f
					};

					static constexpr float sinTable[] = {
						0.0f, 1.0f, 0.0f, -1.0f
					};

					cosAngle = cosTable[quarter];
					sinAngle = sinTable[quarter];
				}
				else {
					const float radians =
						angleDegrees *
						(3.1415926535f / 180.0f);

					cosAngle = std::cos(radians);
					sinAngle = std::sin(radians);
				}

				const float centerX =
					destinationPixels.x +
					0.5f * destinationPixels.w;

				const float centerY =
					destinationPixels.y +
					0.5f * destinationPixels.h;

				const float halfExtentX =
					0.5f *
					(std::fabs(cosAngle) *
						destinationPixels.w *
						legacyElementScaleX +
					std::fabs(sinAngle) *
						destinationPixels.h *
						legacyElementScaleY) /
					legacyElementScaleX;

				const float halfExtentY =
					0.5f *
					(std::fabs(sinAngle) *
						destinationPixels.w *
						legacyElementScaleX +
					std::fabs(cosAngle) *
						destinationPixels.h *
						legacyElementScaleY) /
					legacyElementScaleY;

				if (centerX + halfExtentX < -epsilon ||
					centerY + halfExtentY < -epsilon ||
					centerX - halfExtentX >
					static_cast<float>(outW) + epsilon ||
					centerY - halfExtentY >
					static_cast<float>(outH) + epsilon)
				{
					return true;
				}

				points[0] = {
					destinationPixels.x,
					destinationPixels.y
				};

				points[1] = {
					destinationPixels.x + destinationPixels.w,
					destinationPixels.y
				};

				points[2] = {
					destinationPixels.x + destinationPixels.w,
					destinationPixels.y + destinationPixels.h
				};

				points[3] = {
					destinationPixels.x,
					destinationPixels.y + destinationPixels.h
				};

				for (SDL_FPoint& point : points) {
					const float translatedX =
						(point.x - centerX) *
						legacyElementScaleX;

					const float translatedY =
						(point.y - centerY) *
						legacyElementScaleY;

					point.x =
						(translatedX * cosAngle -
							translatedY * sinAngle) /
						legacyElementScaleX +
						centerX;

					point.y =
						(translatedX * sinAngle +
							translatedY * cosAngle) /
						legacyElementScaleY +
						centerY;
				}
			}

			float u0 =
				static_cast<float>(sourceRect.x) * invTexW;

			float v0 =
				static_cast<float>(sourceRect.y) * invTexH;

			float u1 =
				static_cast<float>(sourceRect.x + sourceRect.w) *
				invTexW;

			float v1 =
				static_cast<float>(sourceRect.y + sourceRect.h) *
				invTexH;

			if (flipHorizontal) {
				std::swap(u0, u1);
			}

			if (flipVertical) {
				std::swap(v0, v1);
			}

			const SDL_Color color = {
				textureR,
				textureG,
				textureB,
				clamp_u8(alpha01)
			};

			SDL_Vertex vertices[4];

			vertices[0] = {
				points[0],
				color,
				{ u0, v0 }
			};

			vertices[1] = {
				points[1],
				color,
				{ u1, v0 }
			};

			vertices[2] = {
				points[2],
				color,
				{ u1, v1 }
			};

			vertices[3] = {
				points[3],
				color,
				{ u0, v1 }
			};

			static constexpr int indices[6] = {
				0, 1, 2,
				0, 2, 3
			};

			return SDL_RenderGeometry(
				renderer_[m],
				texture,
				vertices,
				4,
				indices,
				6
			) == 0;
		};

	auto render_path =
		[&](bool reflect, int kind = -1) -> bool
		{
			/*
			 * Cheap logical-space rejection is only safe for an unrotated
			 * base element. An arbitrarily rotated quad can swing a corner
			 * back into view even when its unrotated rectangle is outside.
			 *
			 * Reflections are also excluded because their derived position
			 * may re-enter the layout even when the base element does not.
			 */
			if (!reflect && viewInfo.Angle == 0.0f) {
				if (dst0.x + dst0.w <= 0.0f ||
					dst0.y + dst0.h <= 0.0f ||
					dst0.x >= static_cast<float>(layoutWidth) ||
					dst0.y >= static_cast<float>(layoutHeight))
				{
					return true;
				}
			}

			SDL_Rect sourceRect = src0;
			SDL_FRect destinationRect = dst0;

			float pathAlpha = alpha;
			bool flipHorizontal = false;
			bool flipVertical = false;

			if (reflect) {
				pathAlpha =
					alpha * viewInfo.ReflectionAlpha;

				flipHorizontal =
					kind == 2 || kind == 3;

				flipVertical =
					kind == 0 || kind == 1;

				switch (kind) {
					case 0:
					destinationRect.h *=
						viewInfo.ReflectionScale;

					destinationRect.y -=
						destinationRect.h +
						viewInfo.ReflectionDistance;

					break;

					case 1:
					destinationRect.y +=
						destinationRect.h +
						viewInfo.ReflectionDistance;

					destinationRect.h *=
						viewInfo.ReflectionScale;

					break;

					case 2:
					destinationRect.w *=
						viewInfo.ReflectionScale;

					destinationRect.x -=
						destinationRect.w +
						viewInfo.ReflectionDistance;

					break;

					case 3:
					destinationRect.x +=
						destinationRect.w +
						viewInfo.ReflectionDistance;

					destinationRect.w *=
						viewInfo.ReflectionScale;

					break;

					default:
					break;
				}
			}

			/*
			 * Preserve explicit component/container clipping first.
			 *
			 * clip_to_rect snapshots the current source/destination pair,
			 * so this also maps reflected geometry correctly.
			 */
			if (hasContainer) {
				clip_to_rect(
					sourceRect,
					destinationRect,
					container
				);
			}

			if (destinationRect.w <= 0.0f ||
				destinationRect.h <= 0.0f ||
				sourceRect.w <= 0 ||
				sourceRect.h <= 0)
			{
				return true;
			}


			return draw_quad(
				sourceRect,
				destinationRect,
				viewInfo.Angle,
				flipHorizontal,
				flipVertical,
				pathAlpha
			);
		};

	bool result = render_path(false);

	if (viewInfo.hasReflection) {
		for (int reflectionKind = 0;
			reflectionKind < 4;
			++reflectionKind)
		{
			if (viewInfo.reflectionMask &
				(1 << reflectionKind))
			{
				result &=
					render_path(
						true,
						reflectionKind
					);
			}
		}
	}

	return result;
}
