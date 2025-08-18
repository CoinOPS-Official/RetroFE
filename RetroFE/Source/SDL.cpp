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
#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif
#include "Utility/Utils.h"

std::vector<SDL_Window*>    SDL::window_;
std::vector<SDL_Renderer*>  SDL::renderer_;
std::vector<SDL::MonitorRT> SDL::renderTargets_;
SDL_mutex* SDL::mutex_ = nullptr;
std::vector<int>            SDL::displayWidth_;
std::vector<int>            SDL::displayHeight_;
std::vector<int>            SDL::windowWidth_;
std::vector<int>            SDL::windowHeight_;
std::vector<bool>           SDL::fullscreen_;
std::vector<int>            SDL::rotation_;
std::vector<bool>           SDL::mirror_;
int                         SDL::numScreens_ = 1;
int                         SDL::numDisplays_ = 1;
int                         SDL::screenCount_;

// Initialize SDL
bool SDL::initialize(Configuration& config) {
	int audioRate = MIX_DEFAULT_FREQUENCY;
	Uint16 audioFormat = MIX_DEFAULT_FORMAT; // 16-bit stereo
	int audioChannels = 2;
	int audioBuffers = 4096;
	bool hideMouse;

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

#ifdef WIN32
	if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
	{
		LOG_ERROR("SDL", "Unable to set DPI awareness hint");
	}
#endif
	if (SDL_WasInit(0) == 0) {
		// First-time startup: Initialize everything.
		LOG_INFO("SDL", "Performing first-time full initialization of all SDL subsystems.");
		if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
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

	SDL_SetHint(SDL_HINT_RENDER_BATCHING, "0"); // For all renderers

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
#elif defined(__APPLE__)
			windowFlags |= SDL_WINDOW_BORDERLESS;
#else
			windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
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
				renderTargets_.resize(screenCount_);

				for (int logicalScreen = 0; logicalScreen < screenCount_; ++logicalScreen) {
					SDL_Renderer* r = renderer_[logicalScreen];
					if (!r) return false;

					int w = windowWidth_[logicalScreen];
					int h = windowHeight_[logicalScreen];

					auto& ring = renderTargets_[logicalScreen];
					ring.width = w; ring.height = h;
					ring.ringCount = 2;         // set to 3 if you want triple buffering

					for (int i = 0; i < ring.ringCount; ++i) {
						SDL_Texture* t = SDL_CreateTexture(
							r,
							SDL_PIXELFORMAT_RGBA32,           // OK alias for RGBA8888
							SDL_TEXTUREACCESS_TARGET,
							w, h);
						if (!t) {
							LOG_ERROR("SDL", std::string("Create render target texture failed: ") + SDL_GetError());
							return false;
						}
						SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
						SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest); // or Linear if you prefer
						ring.rt[i] = t;
					}
				}

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

	mutex_ = SDL_CreateMutex();

	if (mutex_ == nullptr)
	{
		std::string error = SDL_GetError();
		LOG_ERROR("SDL", "Mutex creation failed: " + error);
		return false;
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
	for (auto& ring : renderTargets_) {
		for (int i = 0; i < 3; ++i) {
			if (ring.rt[i]) { SDL_DestroyTexture(ring.rt[i]); ring.rt[i] = nullptr; }
		}
	}
	renderTargets_.clear();

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
		// This is the final application exit. Shut down everything.
		LOG_INFO("SDL", "Performing full de-initialization of all SDL subsystems.");
		Mix_CloseAudio();
		Mix_Quit();
		SDL_Quit();
		
		if (mutex_) {
			SDL_DestroyMutex(mutex_);
			mutex_ = nullptr;
		}
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

	SDL_ShowCursor(SDL_TRUE);

	return true;
}


// Get the renderer
SDL_Renderer* SDL::getRenderer(int index)
{
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

// Get the mutex
SDL_mutex* SDL::getMutex()
{
	return mutex_;
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
	int i = (index < screenCount_) ? index : 0;
	auto& ring = renderTargets_[i];
	return ring.rt[ring.writeIdx];
}

// call after presenting that monitor to advance the ring
void SDL::advanceRenderTarget(int index) {
	if (renderTargets_.empty()) return;
	int i = (index < screenCount_) ? index : 0;
	auto& ring = renderTargets_[i];
	ring.writeIdx = (ring.writeIdx + 1) % ring.ringCount;
}

// Render a copy of a texture
bool SDL::renderCopy(SDL_Texture* texture, float alpha, SDL_Rect const* src, SDL_Rect const* dest, ViewInfo& viewInfo, int layoutWidth, int layoutHeight)
{

	// Skip rendering if the object is invisible anyway or if renderer does not exist
	if (alpha == 0 || viewInfo.Monitor >= screenCount_ || !renderer_[viewInfo.Monitor])
		return true;
	SDL_GetWindowSize(getWindow(viewInfo.Monitor), &windowWidth_[viewInfo.Monitor], &windowHeight_[viewInfo.Monitor]);

	float scaleX = (float)windowWidth_[viewInfo.Monitor] / (float)layoutWidth;
	float scaleY = (float)windowHeight_[viewInfo.Monitor] / (float)layoutHeight;

	// 90 or 270 degree rotation; change scale factors
	if (rotation_[viewInfo.Monitor] % 2 == 1) {
		scaleX = (float)windowHeight_[viewInfo.Monitor] / (float)layoutWidth;
		scaleY = (float)windowWidth_[viewInfo.Monitor] / (float)layoutHeight;
	}

	if (mirror_[viewInfo.Monitor])
		scaleY /= 2;

	// Don't print outside the screen in mirror mode
	if (mirror_[viewInfo.Monitor] && (viewInfo.ContainerWidth < 0 || viewInfo.ContainerHeight < 0)) {
		viewInfo.ContainerX = 0;
		viewInfo.ContainerY = 0;
		viewInfo.ContainerWidth = static_cast<float>(layoutWidth);
		viewInfo.ContainerHeight = static_cast<float>(layoutHeight);
	}

	SDL_Rect srcRect{};
	SDL_Rect dstRect{};
	SDL_Rect srcRectCopy{};
	SDL_Rect dstRectCopy{};
	SDL_Rect srcRectOrig{};
	SDL_Rect dstRectOrig{};
	double   imageScaleX;
	double   imageScaleY;

	dstRect.w = dest->w;
	dstRect.h = dest->h;

	if (fullscreen_[viewInfo.Monitor]) {
		dstRect.x = dest->x + (displayWidth_[viewInfo.Monitor] - windowWidth_[viewInfo.Monitor]) / 2;
		dstRect.y = dest->y + (displayHeight_[viewInfo.Monitor] - windowHeight_[viewInfo.Monitor]) / 2;
	}
	else {
		dstRect.x = dest->x;
		dstRect.y = dest->y;
	}

	// Create the base fields to check against the container.
	if (src) {
		srcRect.x = src->x;
		srcRect.y = src->y;
		srcRect.w = src->w;
		srcRect.h = src->h;
	}
	else {
		srcRect.x = 0;
		srcRect.y = 0;
		int w = 0;
		int h = 0;
		SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);
		srcRect.w = w;
		srcRect.h = h;
	}

	// Define the scale
	imageScaleX = (dstRect.w > 0) ? static_cast<double>(srcRect.w) / static_cast<double>(dstRect.w) : 0.0;
	imageScaleY = (dstRect.h > 0) ? static_cast<double>(srcRect.h) / static_cast<double>(dstRect.h) : 0.0;

	// Make two copies
	srcRectOrig.x = srcRect.x;
	srcRectOrig.y = srcRect.y;
	srcRectOrig.w = srcRect.w;
	srcRectOrig.h = srcRect.h;
	dstRectOrig.x = dstRect.x;
	dstRectOrig.y = dstRect.y;
	dstRectOrig.w = dstRect.w;
	dstRectOrig.h = dstRect.h;

	srcRectCopy.x = srcRect.x;
	srcRectCopy.y = srcRect.y;
	srcRectCopy.w = srcRect.w;
	srcRectCopy.h = srcRect.h;
	dstRectCopy.x = dstRect.x;
	dstRectCopy.y = dstRect.y;
	dstRectCopy.w = dstRect.w;
	dstRectCopy.h = dstRect.h;

	// If a container has been defined, limit the display to the container boundaries.
	if (viewInfo.ContainerWidth > 0 && viewInfo.ContainerHeight > 0 &&
		dstRectCopy.w > 0 && dstRectCopy.h > 0) {

		// Correct if the image falls to the left of the container
		if (dstRect.x < viewInfo.ContainerX) {
			dstRect.x = static_cast<int>(viewInfo.ContainerX);
			dstRect.w = dstRectCopy.w + dstRectCopy.x - dstRect.x;
			srcRect.x = srcRectCopy.x + srcRectCopy.w * (dstRect.x - dstRectCopy.x) / dstRectCopy.w;
		}

		// Correct if the image falls to the right of the container
		if ((dstRectCopy.x + dstRectCopy.w) > (viewInfo.ContainerX + viewInfo.ContainerWidth)) {
			dstRect.w = static_cast<int>(viewInfo.ContainerX + viewInfo.ContainerWidth) - dstRect.x;
		}

		// Correct if the image falls to the top of the container
		if (dstRect.y < viewInfo.ContainerY) {
			dstRect.y = static_cast<int>(viewInfo.ContainerY);
			dstRect.h = dstRectCopy.h + dstRectCopy.y - dstRect.y;
			srcRect.y = srcRectCopy.y + srcRectCopy.h * (dstRect.y - dstRectCopy.y) / dstRectCopy.h;
		}

		// Correct if the image falls to the bottom of the container
		if ((dstRectCopy.y + dstRectCopy.h) > (viewInfo.ContainerY + viewInfo.ContainerHeight)) {
			dstRect.h = static_cast<int>(viewInfo.ContainerY + viewInfo.ContainerHeight) - dstRect.y;
		}

		// Define source width and height
		srcRect.w = static_cast<int>(dstRect.w * imageScaleX);
		srcRect.h = static_cast<int>(dstRect.h * imageScaleY);

	}

	double angle = viewInfo.Angle;
	if (!mirror_[viewInfo.Monitor])
		angle += rotation_[viewInfo.Monitor] * 90;

	dstRect.x = (int)(dstRect.x * scaleX);
	dstRect.y = (int)(dstRect.y * scaleY);
	dstRect.w = (int)(dstRect.w * scaleX);
	dstRect.h = (int)(dstRect.h * scaleY);

	if (mirror_[viewInfo.Monitor]) {
		if (rotation_[viewInfo.Monitor] % 2 == 0) {
			if (srcRect.h > 0 && srcRect.w > 0) {
				dstRect.y += windowHeight_[viewInfo.Monitor] / 2;
				SDL_SetTextureAlphaMod(texture, static_cast<char>(alpha * 255));
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_NONE);
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
				dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
				angle += 180;
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_NONE);
			}
		}
		else {
			if (srcRect.h > 0 && srcRect.w > 0) {
				int tmp = dstRect.x;
				dstRect.x = windowWidth_[viewInfo.Monitor] / 2 - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
				angle += 90;
				SDL_SetTextureAlphaMod(texture, static_cast<char>(alpha * 255));
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_NONE);
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
				dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
				angle += 180;
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_NONE);
			}
		}
	}
	else {
		// 90 degree rotation
		if (rotation_[viewInfo.Monitor] == 1) {
			int tmp = dstRect.x;
			dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
			dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
		}
		// 180 degree rotation
		if (rotation_[viewInfo.Monitor] == 2) {
			dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
			dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
		}
		// 270 degree rotation
		if (rotation_[viewInfo.Monitor] == 3) {
			int tmp = dstRect.x;
			dstRect.x = dstRect.y + dstRect.h / 2 - dstRect.w / 2;
			dstRect.y = windowHeight_[viewInfo.Monitor] - tmp - dstRect.h / 2 - dstRect.w / 2;
		}

		if (srcRect.h > 0 && srcRect.w > 0) {
			SDL_SetTextureAlphaMod(texture, static_cast<char>(alpha * 255));
			SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_NONE);
		}
	}

	// Restore original parameters
	srcRect.x = srcRectOrig.x;
	srcRect.y = srcRectOrig.y;
	srcRect.w = srcRectOrig.w;
	srcRect.h = srcRectOrig.h;
	dstRect.x = dstRectOrig.x;
	dstRect.y = dstRectOrig.y;
	dstRect.w = dstRectOrig.w;
	dstRect.h = dstRectOrig.h;
	srcRectCopy.x = srcRectOrig.x;
	srcRectCopy.y = srcRectOrig.y;
	srcRectCopy.w = srcRectOrig.w;
	srcRectCopy.h = srcRectOrig.h;
	dstRectCopy.x = dstRectOrig.x;
	dstRectCopy.y = dstRectOrig.y;
	dstRectCopy.w = dstRectOrig.w;
	dstRectCopy.h = dstRectOrig.h;

	if (viewInfo.Reflection.find("top") != std::string::npos) {
		dstRect.h = static_cast<unsigned int>(static_cast<float>(dstRect.h) * viewInfo.ReflectionScale);
		dstRect.y = dstRect.y - dstRect.h - viewInfo.ReflectionDistance;
		imageScaleY = (dstRect.h > 0) ? static_cast<double>(srcRect.h) / static_cast<double>(dstRect.h) : 0.0;
		dstRectCopy.y = dstRect.y;
		dstRectCopy.h = dstRect.h;

		// If a container has been defined, limit the display to the container boundaries.
		if (viewInfo.ContainerWidth > 0 && viewInfo.ContainerHeight > 0 &&
			dstRectCopy.w > 0 && dstRectCopy.h > 0) {

			// Correct if the image falls to the left of the container
			if (dstRect.x < viewInfo.ContainerX) {
				dstRect.x = static_cast<int>(viewInfo.ContainerX);
				dstRect.w = dstRectCopy.w + dstRectCopy.x - dstRect.x;
				srcRect.x = srcRectCopy.x + srcRectCopy.w * (dstRect.x - dstRectCopy.x) / dstRectCopy.w;
			}

			// Correct if the image falls to the right of the container
			if ((dstRectCopy.x + dstRectCopy.w) > (viewInfo.ContainerX + viewInfo.ContainerWidth)) {
				dstRect.w = static_cast<int>(viewInfo.ContainerX + viewInfo.ContainerWidth) - dstRect.x;
			}

			// Correct if the image falls to the top of the container
			if (dstRect.y < viewInfo.ContainerY) {
				dstRect.y = static_cast<int>(viewInfo.ContainerY);
				dstRect.h = dstRectCopy.h + dstRectCopy.y - dstRect.y;
			}

			// Correct if the image falls to the bottom of the container
			if ((dstRectCopy.y + dstRectCopy.h) > (viewInfo.ContainerY + viewInfo.ContainerHeight)) {
				dstRect.h = static_cast<int>(viewInfo.ContainerY + viewInfo.ContainerHeight) - dstRect.y;
				srcRect.y = srcRectCopy.y + srcRectCopy.h * (dstRectCopy.h - dstRect.h) / dstRectCopy.h;
			}

			// Define source width and height
			srcRect.w = static_cast<int>(dstRect.w * imageScaleX);
			srcRect.h = static_cast<int>(dstRect.h * imageScaleY);

		}

		angle = viewInfo.Angle;
		if (!mirror_[viewInfo.Monitor])
			angle += rotation_[viewInfo.Monitor] * 90;

		dstRect.x = (int)(dstRect.x * scaleX);
		dstRect.y = (int)(dstRect.y * scaleY);
		dstRect.w = (int)(dstRect.w * scaleX);
		dstRect.h = (int)(dstRect.h * scaleY);

		if (mirror_[viewInfo.Monitor]) {
			if (rotation_[viewInfo.Monitor] % 2 == 0) {
				if (srcRect.h > 0 && srcRect.w > 0) {
					dstRect.y += windowHeight_[viewInfo.Monitor] / 2;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
				}
			}
			else {
				if (srcRect.h > 0 && srcRect.w > 0) {
					int tmp = dstRect.x;
					dstRect.x = windowWidth_[viewInfo.Monitor] / 2 - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
					dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
					angle += 90;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
				}
			}
		}
		else {
			// 90 degree rotation
			if (rotation_[viewInfo.Monitor] == 1) {
				int tmp = dstRect.x;
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
			}
			// 180 degree rotation
			if (rotation_[viewInfo.Monitor] == 2) {
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
				dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
			}
			// 270 degree rotation
			if (rotation_[viewInfo.Monitor] == 3) {
				int tmp = dstRect.x;
				dstRect.x = dstRect.y + dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = windowHeight_[viewInfo.Monitor] - tmp - dstRect.h / 2 - dstRect.w / 2;
			}
			if (srcRect.h > 0 && srcRect.w > 0) {
				SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
			}
		}
	}

	// Restore original parameters
	srcRect.x = srcRectOrig.x;
	srcRect.y = srcRectOrig.y;
	srcRect.w = srcRectOrig.w;
	srcRect.h = srcRectOrig.h;
	dstRect.x = dstRectOrig.x;
	dstRect.y = dstRectOrig.y;
	dstRect.w = dstRectOrig.w;
	dstRect.h = dstRectOrig.h;
	srcRectCopy.x = srcRectOrig.x;
	srcRectCopy.y = srcRectOrig.y;
	srcRectCopy.w = srcRectOrig.w;
	srcRectCopy.h = srcRectOrig.h;
	dstRectCopy.x = dstRectOrig.x;
	dstRectCopy.y = dstRectOrig.y;
	dstRectCopy.w = dstRectOrig.w;
	dstRectCopy.h = dstRectOrig.h;

	if (viewInfo.Reflection.find("bottom") != std::string::npos) {
		dstRect.y = dstRect.y + dstRect.h + viewInfo.ReflectionDistance;
		dstRect.h = static_cast<unsigned int>(static_cast<float>(dstRect.h) * viewInfo.ReflectionScale);
		imageScaleY = (dstRect.h > 0) ? static_cast<double>(srcRect.h) / static_cast<double>(dstRect.h) : 0.0;
		dstRectCopy.y = dstRect.y;
		dstRectCopy.h = dstRect.h;

		// If a container has been defined, limit the display to the container boundaries.
		if (viewInfo.ContainerWidth > 0 && viewInfo.ContainerHeight > 0 &&
			dstRectCopy.w > 0 && dstRectCopy.h > 0) {

			// Correct if the image falls to the left of the container
			if (dstRect.x < viewInfo.ContainerX) {
				dstRect.x = static_cast<int>(viewInfo.ContainerX);
				dstRect.w = dstRectCopy.w + dstRectCopy.x - dstRect.x;
				srcRect.x = srcRectCopy.x + srcRectCopy.w * (dstRect.x - dstRectCopy.x) / dstRectCopy.w;
			}
			// Correct if the image falls to the right of the container
			if ((dstRectCopy.x + dstRectCopy.w) > (viewInfo.ContainerX + viewInfo.ContainerWidth)) {
				dstRect.w = static_cast<int>(viewInfo.ContainerX + viewInfo.ContainerWidth) - dstRect.x;
			}
			// Correct if the image falls to the top of the container
			if (dstRect.y < viewInfo.ContainerY) {
				dstRect.y = static_cast<int>(viewInfo.ContainerY);
				dstRect.h = dstRectCopy.h + dstRectCopy.y - dstRect.y;
			}
			// Correct if the image falls to the bottom of the container
			if ((dstRectCopy.y + dstRectCopy.h) > (viewInfo.ContainerY + viewInfo.ContainerHeight)) {
				dstRect.h = static_cast<int>(viewInfo.ContainerY + viewInfo.ContainerHeight) - dstRect.y;
				srcRect.y = srcRectCopy.y + srcRectCopy.h * (dstRectCopy.h - dstRect.h) / dstRectCopy.h;
			}
			// Define source width and height
			srcRect.w = static_cast<int>(dstRect.w * imageScaleX);
			srcRect.h = static_cast<int>(dstRect.h * imageScaleY);
		}

		angle = viewInfo.Angle;
		if (!mirror_[viewInfo.Monitor])
			angle += rotation_[viewInfo.Monitor] * 90;

		dstRect.x = (int)(dstRect.x * scaleX);
		dstRect.y = (int)(dstRect.y * scaleY);
		dstRect.w = (int)(dstRect.w * scaleX);
		dstRect.h = (int)(dstRect.h * scaleY);

		if (mirror_[viewInfo.Monitor]) {
			if (rotation_[viewInfo.Monitor] % 2 == 0) {
				if (srcRect.h > 0 && srcRect.w > 0) {
					dstRect.y += windowHeight_[viewInfo.Monitor] / 2;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
				}
			}
			else {
				if (srcRect.h > 0 && srcRect.w > 0) {
					int tmp = dstRect.x;
					dstRect.x = windowWidth_[viewInfo.Monitor] / 2 - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
					dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
					angle += 90;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
				}
			}
		}
		else {
			// 90 degree rotation
			if (rotation_[viewInfo.Monitor] == 1) {
				int tmp = dstRect.x;
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
			}
			// 180 degree rotation
			if (rotation_[viewInfo.Monitor] == 2) {
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
				dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
			}
			// 270 degree rotation
			if (rotation_[viewInfo.Monitor] == 3) {
				int tmp = dstRect.x;
				dstRect.x = dstRect.y + dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = windowHeight_[viewInfo.Monitor] - tmp - dstRect.h / 2 - dstRect.w / 2;
			}
			if (srcRect.h > 0 && srcRect.w > 0) {
				SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_VERTICAL);
			}
		}
	}

	// Restore original parameters
	srcRect.x = srcRectOrig.x;
	srcRect.y = srcRectOrig.y;
	srcRect.w = srcRectOrig.w;
	srcRect.h = srcRectOrig.h;
	dstRect.x = dstRectOrig.x;
	dstRect.y = dstRectOrig.y;
	dstRect.w = dstRectOrig.w;
	dstRect.h = dstRectOrig.h;
	srcRectCopy.x = srcRectOrig.x;
	srcRectCopy.y = srcRectOrig.y;
	srcRectCopy.w = srcRectOrig.w;
	srcRectCopy.h = srcRectOrig.h;
	dstRectCopy.x = dstRectOrig.x;
	dstRectCopy.y = dstRectOrig.y;
	dstRectCopy.w = dstRectOrig.w;
	dstRectCopy.h = dstRectOrig.h;

	if (viewInfo.Reflection.find("left") != std::string::npos) {
		dstRect.w = static_cast<unsigned int>(static_cast<float>(dstRect.w) * viewInfo.ReflectionScale);
		dstRect.x = dstRect.x - dstRect.w - viewInfo.ReflectionDistance;
		imageScaleX = (dstRect.h > 0) ? static_cast<double>(srcRect.w) / static_cast<double>(dstRect.w) : 0.0;
		dstRectCopy.x = dstRect.x;
		dstRectCopy.w = dstRect.w;

		// If a container has been defined, limit the display to the container boundaries.
		if (viewInfo.ContainerWidth > 0 && viewInfo.ContainerHeight > 0 &&
			dstRectCopy.w > 0 && dstRectCopy.h > 0) {
			// Correct if the image falls to the left of the container
			if (dstRect.x < viewInfo.ContainerX) {
				dstRect.x = static_cast<int>(viewInfo.ContainerX);
				dstRect.w = dstRectCopy.w + dstRectCopy.x - dstRect.x;
			}
			// Correct if the image falls to the right of the container
			if ((dstRectCopy.x + dstRectCopy.w) > (viewInfo.ContainerX + viewInfo.ContainerWidth)) {
				dstRect.w = static_cast<int>(viewInfo.ContainerX + viewInfo.ContainerWidth) - dstRect.x;
				srcRect.x = srcRectCopy.x + srcRectCopy.w * (dstRectCopy.w - dstRect.w) / dstRectCopy.w;
			}
			// Correct if the image falls to the top of the container
			if (dstRect.y < viewInfo.ContainerY) {
				dstRect.y = static_cast<int>(viewInfo.ContainerY);
				dstRect.h = dstRectCopy.h + dstRectCopy.y - dstRect.y;
				srcRect.y = srcRectCopy.y + srcRectCopy.h * (dstRect.y - dstRectCopy.y) / dstRectCopy.h;
			}
			// Correct if the image falls to the bottom of the container
			if ((dstRectCopy.y + dstRectCopy.h) > (viewInfo.ContainerY + viewInfo.ContainerHeight)) {
				dstRect.h = static_cast<int>(viewInfo.ContainerY + viewInfo.ContainerHeight) - dstRect.y;
			}
			// Define source width and height
			srcRect.w = static_cast<int>(dstRect.w * imageScaleX);
			srcRect.h = static_cast<int>(dstRect.h * imageScaleY);

		}

		angle = viewInfo.Angle;
		if (!mirror_[viewInfo.Monitor])
			angle += rotation_[viewInfo.Monitor] * 90;

		dstRect.x = (int)(dstRect.x * scaleX);
		dstRect.y = (int)(dstRect.y * scaleY);
		dstRect.w = (int)(dstRect.w * scaleX);
		dstRect.h = (int)(dstRect.h * scaleY);

		if (mirror_[viewInfo.Monitor]) {
			if (rotation_[viewInfo.Monitor] % 2 == 0) {
				if (srcRect.h > 0 && srcRect.w > 0) {
					dstRect.y += windowHeight_[viewInfo.Monitor] / 2;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
				}
			}
			else {
				if (srcRect.h > 0 && srcRect.w > 0) {
					int tmp = dstRect.x;
					dstRect.x = windowWidth_[viewInfo.Monitor] / 2 - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
					dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
					angle += 90;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
				}
			}
		}
		else {
			// 90 degree rotation
			if (rotation_[viewInfo.Monitor] == 1) {
				int tmp = dstRect.x;
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
			}
			// 180 degree rotation
			if (rotation_[viewInfo.Monitor] == 2) {
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
				dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
			}
			// 270 degree rotation
			if (rotation_[viewInfo.Monitor] == 3) {
				int tmp = dstRect.x;
				dstRect.x = dstRect.y + dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = windowHeight_[viewInfo.Monitor] - tmp - dstRect.h / 2 - dstRect.w / 2;
			}
			if (srcRect.h > 0 && srcRect.w > 0) {
				SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
			}
		}
	}

	// Restore original parameters
	srcRect.x = srcRectOrig.x;
	srcRect.y = srcRectOrig.y;
	srcRect.w = srcRectOrig.w;
	srcRect.h = srcRectOrig.h;
	dstRect.x = dstRectOrig.x;
	dstRect.y = dstRectOrig.y;
	dstRect.w = dstRectOrig.w;
	dstRect.h = dstRectOrig.h;
	srcRectCopy.x = srcRectOrig.x;
	srcRectCopy.y = srcRectOrig.y;
	srcRectCopy.w = srcRectOrig.w;
	srcRectCopy.h = srcRectOrig.h;
	dstRectCopy.x = dstRectOrig.x;
	dstRectCopy.y = dstRectOrig.y;
	dstRectCopy.w = dstRectOrig.w;
	dstRectCopy.h = dstRectOrig.h;

	if (viewInfo.Reflection.find("right") != std::string::npos) {
		dstRect.x = dstRect.x + dstRect.w + viewInfo.ReflectionDistance;
		dstRect.w = static_cast<unsigned int>(static_cast<float>(dstRect.w) * viewInfo.ReflectionScale);
		imageScaleX = (dstRect.h > 0) ? static_cast<double>(srcRect.w) / static_cast<double>(dstRect.w) : 0.0;
		dstRectCopy.x = dstRect.x;
		dstRectCopy.w = dstRect.w;

		// If a container has been defined, limit the display to the container boundaries.
		if (viewInfo.ContainerWidth > 0 && viewInfo.ContainerHeight > 0 &&
			dstRectCopy.w > 0 && dstRectCopy.h > 0) {
			// Correct if the image falls to the left of the container
			if (dstRect.x < viewInfo.ContainerX) {
				dstRect.x = static_cast<int>(viewInfo.ContainerX);
				dstRect.w = dstRectCopy.w + dstRectCopy.x - dstRect.x;
			}
			// Correct if the image falls to the right of the container
			if ((dstRectCopy.x + dstRectCopy.w) > (viewInfo.ContainerX + viewInfo.ContainerWidth)) {
				dstRect.w = static_cast<int>(viewInfo.ContainerX + viewInfo.ContainerWidth) - dstRect.x;
				srcRect.x = srcRectCopy.x + srcRectCopy.w * (dstRectCopy.w - dstRect.w) / dstRectCopy.w;
			}
			// Correct if the image falls to the top of the container
			if (dstRect.y < viewInfo.ContainerY) {
				dstRect.y = static_cast<int>(viewInfo.ContainerY);
				dstRect.h = dstRectCopy.h + dstRectCopy.y - dstRect.y;
				srcRect.y = srcRectCopy.y + srcRectCopy.h * (dstRect.y - dstRectCopy.y) / dstRectCopy.h;
			}
			// Correct if the image falls to the bottom of the container
			if ((dstRectCopy.y + dstRectCopy.h) > (viewInfo.ContainerY + viewInfo.ContainerHeight)) {
				dstRect.h = static_cast<int>(viewInfo.ContainerY + viewInfo.ContainerHeight) - dstRect.y;
			}
			// Define source width and height
			srcRect.w = static_cast<int>(dstRect.w * imageScaleX);
			srcRect.h = static_cast<int>(dstRect.h * imageScaleY);

		}

		angle = viewInfo.Angle;
		if (!mirror_[viewInfo.Monitor])
			angle += rotation_[viewInfo.Monitor] * 90;

		dstRect.x = (int)(dstRect.x * scaleX);
		dstRect.y = (int)(dstRect.y * scaleY);
		dstRect.w = (int)(dstRect.w * scaleX);
		dstRect.h = (int)(dstRect.h * scaleY);

		if (mirror_[viewInfo.Monitor]) {
			if (rotation_[viewInfo.Monitor] % 2 == 0) {
				if (srcRect.h > 0 && srcRect.w > 0) {
					dstRect.y += windowHeight_[viewInfo.Monitor] / 2;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
				}
			}
			else {
				if (srcRect.h > 0 && srcRect.w > 0) {
					int tmp = dstRect.x;
					dstRect.x = windowWidth_[viewInfo.Monitor] / 2 - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
					dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
					angle += 90;
					SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
					dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
					dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
					angle += 180;
					SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
				}
			}
		}
		else {
			// 90 degree rotation
			if (rotation_[viewInfo.Monitor] == 1) {
				int tmp = dstRect.x;
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.y - dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = tmp - dstRect.h / 2 + dstRect.w / 2;
			}
			// 180 degree rotation
			if (rotation_[viewInfo.Monitor] == 2) {
				dstRect.x = windowWidth_[viewInfo.Monitor] - dstRect.x - dstRect.w;
				dstRect.y = windowHeight_[viewInfo.Monitor] - dstRect.y - dstRect.h;
			}
			// 270 degree rotation
			if (rotation_[viewInfo.Monitor] == 3) {
				int tmp = dstRect.x;
				dstRect.x = dstRect.y + dstRect.h / 2 - dstRect.w / 2;
				dstRect.y = windowHeight_[viewInfo.Monitor] - tmp - dstRect.h / 2 - dstRect.w / 2;
			}
			if (srcRect.h > 0 && srcRect.w > 0) {
				SDL_SetTextureAlphaMod(texture, static_cast<char>(viewInfo.ReflectionAlpha * alpha * 255));
				SDL_RenderCopyEx(renderer_[viewInfo.Monitor], texture, &srcRect, &dstRect, angle, nullptr, SDL_FLIP_HORIZONTAL);
			}
		}
	}
	return true;
}

// SDL 2.0.18+ for SDL_RenderGeometry
bool SDL::renderCopyF(SDL_Texture* texture,
	float alpha,
	const SDL_Rect* src,
	const SDL_FRect* dest,
	ViewInfo& viewInfo,
	int layoutWidth,
	int layoutHeight) {
	// --- Quick rejects ---
	if (!texture) return false;
	if (alpha <= 0.0f) return true;
	const int m = viewInfo.Monitor;
	if (m < 0 || m >= screenCount_ || !renderer_[m]) return true;

	// Output size (pixels)
	int outW = 0, outH = 0;
	if (SDL_GetRendererOutputSize(renderer_[m], &outW, &outH) != 0) {
		SDL_GetWindowSize(getWindow(m), &outW, &outH); // fallback
	}

	// Logical -> pixel scales
	float scaleX = layoutWidth > 0 ? float(outW) / float(layoutWidth) : 1.0f;
	float scaleY = layoutHeight > 0 ? float(outH) / float(layoutHeight) : 1.0f;
	if ((rotation_[m] & 1) == 1) {
		scaleX = layoutWidth > 0 ? float(outH) / float(layoutWidth) : 1.0f;
		scaleY = layoutHeight > 0 ? float(outW) / float(layoutHeight) : 1.0f;
	}
	if (mirror_[m]) scaleY /= 2.0f;

	// Local container (don’t mutate viewInfo)
	SDL_FRect container{};
	bool hasContainer = (viewInfo.ContainerWidth > 0.0f && viewInfo.ContainerHeight > 0.0f);
	if (mirror_[m] && !hasContainer) {
		container = { 0.0f, 0.0f, float(layoutWidth), float(layoutHeight) };
		hasContainer = true;
	}
	else if (hasContainer) {
		container = { viewInfo.ContainerX, viewInfo.ContainerY,
					  viewInfo.ContainerWidth, viewInfo.ContainerHeight };
	}

	// --- Texture size for UVs: prefer ViewInfo.ImageWidth/Height ---
	int texW = (viewInfo.ImageWidth > 0.f) ? int(viewInfo.ImageWidth) : 0;
	int texH = (viewInfo.ImageHeight > 0.f) ? int(viewInfo.ImageHeight) : 0;
	if (texW <= 0 || texH <= 0) {
		// Fallback only if ViewInfo didn’t have it
		SDL_QueryTexture(texture, nullptr, nullptr, &texW, &texH);
	}

	// --- Source / Destination (logical space) ---
	SDL_Rect  srcRect = src ? *src : SDL_Rect{ 0, 0, texW, texH };
	SDL_FRect dstRect = *dest;

	// Fullscreen offset in LOGICAL units
	if (fullscreen_[m]) {
		const float dxL = 0.5f * float(displayWidth_[m] - outW) / std::max(scaleX, 1e-6f);
		const float dyL = 0.5f * float(displayHeight_[m] - outH) / std::max(scaleY, 1e-6f);
		dstRect.x += dxL; dstRect.y += dyL;
	}

	if (dstRect.w <= 0.f || dstRect.h <= 0.f || srcRect.w <= 0 || srcRect.h <= 0)
		return true;

	const SDL_Rect  src0 = srcRect;
	const SDL_FRect dst0 = dstRect;

	// ---- helpers ----
	auto clamp_int = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
	auto clamp_u8 = [&](float a01)->Uint8 {
		if (a01 < 0.f) a01 = 0.f; else if (a01 > 1.f) a01 = 1.f;
		int v = (int)lroundf(a01 * 255.f);
		return (Uint8)clamp_int(v, 0, 255);
		};
	auto to_pixels = [&](SDL_FRect r)->SDL_FRect { r.x *= scaleX; r.y *= scaleY; r.w *= scaleX; r.h *= scaleY; return r; };

	auto clamp_src_to_rect = [&](SDL_Rect& s, const SDL_Rect& limit) {
		const int minX = limit.x, minY = limit.y, maxX = limit.x + limit.w, maxY = limit.y + limit.h;
		if (s.x < minX) s.x = minX;
		if (s.y < minY) s.y = minY;
		if (s.x + s.w > maxX) s.w = std::max(0, maxX - s.x);
		if (s.y + s.h > maxY) s.h = std::max(0, maxY - s.y);
		};

	auto recompute_src_from_dst = [&](SDL_Rect& s, const SDL_Rect& sCopy,
		const SDL_FRect& d, const SDL_FRect& dCopy) {
			const float sx = (dCopy.w > 0.f) ? float(sCopy.w) / dCopy.w : 0.f;
			const float sy = (dCopy.h > 0.f) ? float(sCopy.h) / dCopy.h : 0.f;
			s.w = (int)lroundf(d.w * sx);
			s.h = (int)lroundf(d.h * sy);
			if (dCopy.w > 0.f) s.x = sCopy.x + (int)lroundf((d.x - dCopy.x) * sx);
			if (dCopy.h > 0.f) s.y = sCopy.y + (int)lroundf((d.y - dCopy.y) * sy);
			clamp_src_to_rect(s, src ? *src : SDL_Rect{ 0,0,texW,texH });
		};

	auto clip_to_container = [&](SDL_Rect& s, SDL_FRect& d, const SDL_Rect& sCopy, const SDL_FRect& dCopy) {
		if (!hasContainer || dCopy.w <= 0.f || dCopy.h <= 0.f) return;
		if (d.x < container.x) { const float newW = dCopy.w + dCopy.x - container.x; d.x = container.x; d.w = std::max(0.0f, newW); }
		if ((d.x + d.w) > (container.x + container.w)) d.w = std::max(0.0f, (container.x + container.w) - d.x);
		if (d.y < container.y) { const float newH = dCopy.h + dCopy.y - container.y; d.y = container.y; d.h = std::max(0.0f, newH); }
		if ((d.y + d.h) > (container.y + container.h)) d.h = std::max(0.0f, (container.y + container.h) - d.y);
		recompute_src_from_dst(s, sCopy, d, dCopy);
		};

	auto apply_output_rotation_rect = [&](SDL_FRect& dPx) {
		switch (rotation_[m] & 3) {
			case 0: break;
			case 1: {
				float tmp = dPx.x;
				dPx.x = float(outW) - dPx.y - dPx.h * 0.5f - dPx.w * 0.5f;
				dPx.y = tmp - dPx.h * 0.5f + dPx.w * 0.5f;
			} break;
			case 2: dPx.x = float(outW) - dPx.x - dPx.w;
				dPx.y = float(outH) - dPx.y - dPx.h; break;
			case 3: {
				float tmp = dPx.x;
				dPx.x = dPx.y + dPx.h * 0.5f - dPx.w * 0.5f;
				dPx.y = float(outH) - tmp - dPx.h * 0.5f - dPx.w * 0.5f;
			} break;
		}
		};

	auto rect_to_points = [](const SDL_FRect& r, SDL_FPoint p[4]) {
		p[0] = { r.x,         r.y }; // TL
		p[1] = { r.x + r.w,   r.y }; // TR
		p[2] = { r.x + r.w,   r.y + r.h }; // BR
		p[3] = { r.x,         r.y + r.h }; // BL
		};
	auto rotate_points_about_center = [](SDL_FPoint p[4], const SDL_FRect& r, float angleDeg) {
		if (angleDeg == 0.0f) return;
		const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
		const float rad = angleDeg * 3.1415926535f / 180.0f;
		const float c = cosf(rad), s = sinf(rad);
		for (int i = 0; i < 4; ++i) {
			const float x = p[i].x - cx, y = p[i].y - cy;
			p[i].x = x * c - y * s + cx;
			p[i].y = x * s + y * c + cy;
		}
		};

	auto make_verts = [&](const SDL_Rect& s, SDL_FRect dPx, float angleDeg,
		bool flipH, bool flipV, float alpha01, SDL_Vertex out[4]) {
			// UVs
			float u0 = float(s.x) / float(texW);
			float v0 = float(s.y) / float(texH);
			float u1 = float(s.x + s.w) / float(texW);
			float v1 = float(s.y + s.h) / float(texH);
			if (flipH) std::swap(u0, u1);
			if (flipV) std::swap(v0, v1);

			SDL_FPoint pts[4]; rect_to_points(dPx, pts);
			rotate_points_about_center(pts, dPx, angleDeg);

			const SDL_Color col = { 255,255,255, clamp_u8(alpha01) };
			out[0] = { {pts[0].x, pts[0].y}, col, {u0, v0} };
			out[1] = { {pts[1].x, pts[1].y}, col, {u1, v0} };
			out[2] = { {pts[2].x, pts[2].y}, col, {u1, v1} };
			out[3] = { {pts[3].x, pts[3].y}, col, {u0, v1} };
		};

	auto draw_quad = [&](const SDL_Rect& s, const SDL_FRect& dPx, float angleDeg,
		bool flipH, bool flipV, float alpha01)->bool
		{
			SDL_Vertex v[4];
			make_verts(s, dPx, angleDeg, flipH, flipV, alpha01, v);
			static const int idx[6] = { 0,1,2, 0,2,3 };
			return SDL_RenderGeometry(renderer_[m], texture, v, 4, idx, 6) == 0;
		};

	// --- Base draw path (no reflection) ---
	auto draw_base = [&]()->bool {
		SDL_Rect  s = src0;
		SDL_FRect d = dst0;

		const SDL_Rect  sCopy = s;
		const SDL_FRect dCopy = d;
		clip_to_container(s, d, sCopy, dCopy);
		if (d.w <= 0.f || d.h <= 0.f || s.w <= 0 || s.h <= 0) return true;

		float angle = viewInfo.Angle;
		if (!mirror_[m]) angle += float(rotation_[m] * 90);

		SDL_FRect dPx = to_pixels(d);

		bool ok = true;
		if (mirror_[m]) {
			if ((rotation_[m] & 1) == 0) {
				SDL_FRect r = dPx;
				r.y += float(outH) * 0.5f;
				ok &= draw_quad(s, r, angle, false, false, alpha);
				r.x = float(outW) - r.x - r.w;
				r.y = float(outH) - r.y - r.h;
				ok &= draw_quad(s, r, angle + 180.0f, false, false, alpha);
			}
			else {
				SDL_FRect r = dPx;
				float tmpx = r.x;
				r.x = float(outW) * 0.5f - r.y - r.h * 0.5f - r.w * 0.5f;
				r.y = tmpx - r.h * 0.5f + r.w * 0.5f;
				ok &= draw_quad(s, r, angle + 90.0f, false, false, alpha);
				r.x = float(outW) - r.x - r.w;
				r.y = float(outH) - r.y - r.h;
				ok &= draw_quad(s, r, angle + 270.0f, false, false, alpha);
			}
		}
		else {
			apply_output_rotation_rect(dPx);
			ok &= draw_quad(s, dPx, angle, false, false, alpha);
		}
		return ok;
		};

	// Reflection draw (0=top,1=bottom,2=left,3=right)
	auto draw_reflection = [&](int kind)->bool {
		SDL_Rect  s = src0;
		SDL_FRect d = dst0;

		switch (kind) {
			case 0: d.h = d.h * viewInfo.ReflectionScale;
				d.y = d.y - d.h - viewInfo.ReflectionDistance; break;
			case 1: d.y = d.y + d.h + viewInfo.ReflectionDistance;
				d.h = std::max(0.0f, d.h * viewInfo.ReflectionScale); break;
			case 2: d.w = std::max(0.0f, d.w * viewInfo.ReflectionScale);
				d.x = d.x - d.w - viewInfo.ReflectionDistance; break;
			case 3: d.x = d.x + d.w + viewInfo.ReflectionDistance;
				d.w = std::max(0.0f, d.w * viewInfo.ReflectionScale); break;
		}

		const SDL_Rect  sCopy = s;
		const SDL_FRect dCopy = d;
		clip_to_container(s, d, sCopy, dCopy);
		if (d.w <= 0.f || d.h <= 0.f || s.w <= 0 || s.h <= 0) return true;

		float baseAngle = viewInfo.Angle;
		if (!mirror_[m]) baseAngle += float(rotation_[m] * 90);

		const bool flipV = (kind == 0 || kind == 1);
		const bool flipH = (kind == 2 || kind == 3);
		const float aRef = viewInfo.ReflectionAlpha * alpha;

		SDL_FRect dPx = to_pixels(d);

		bool ok = true;
		if (mirror_[m]) {
			if ((rotation_[m] & 1) == 0) {
				SDL_FRect r = dPx;
				r.y += float(outH) * 0.5f;
				ok &= draw_quad(s, r, baseAngle, flipH, flipV, aRef);
				r.x = float(outW) - r.x - r.w;
				r.y = float(outH) - r.y - r.h;
				ok &= draw_quad(s, r, baseAngle + 180.0f, flipH, flipV, aRef);
			}
			else {
				SDL_FRect r = dPx;
				float tmpx = r.x;
				r.x = float(outW) * 0.5f - r.y - r.h * 0.5f - r.w * 0.5f;
				r.y = tmpx - r.h * 0.5f + r.w * 0.5f;
				ok &= draw_quad(s, r, baseAngle + 90.0f, flipH, flipV, aRef);
				r.x = float(outW) - r.x - r.w;
				r.y = float(outH) - r.y - r.h;
				ok &= draw_quad(s, r, baseAngle + 270.0f, flipH, flipV, aRef);
			}
		}
		else {
			apply_output_rotation_rect(dPx);
			ok &= draw_quad(s, dPx, baseAngle, flipH, flipV, aRef);
		}
		return ok;
		};

	bool ok = true;
	ok &= draw_base();

	const bool wantTop = (viewInfo.Reflection.find("top") != std::string::npos);
	const bool wantBottom = (viewInfo.Reflection.find("bottom") != std::string::npos);
	const bool wantLeft = (viewInfo.Reflection.find("left") != std::string::npos);
	const bool wantRight = (viewInfo.Reflection.find("right") != std::string::npos);

	if (wantTop)    ok &= draw_reflection(0);
	if (wantBottom) ok &= draw_reflection(1);
	if (wantLeft)   ok &= draw_reflection(2);
	if (wantRight)  ok &= draw_reflection(3);

	return ok;
}

