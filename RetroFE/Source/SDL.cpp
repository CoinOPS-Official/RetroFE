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
#include <cstddef>

std::vector<SDL_Window*>    SDL::window_;
std::vector<SDL_Renderer*>  SDL::renderer_;
static std::vector<SDL_Texture*> renderTargets_;
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

namespace {
	struct GeometryBatchState {
		bool active = false;
		bool succeeded = true;
		SDL_Renderer* renderer = nullptr;
		SDL_Texture* texture = nullptr;
		std::vector<SDL_Vertex> vertices;
		std::vector<int> indices;
	};

	GeometryBatchState geometryBatch;

	bool flushGeometryBatch() {
		if (geometryBatch.vertices.empty()) {
			geometryBatch.renderer = nullptr;
			geometryBatch.texture = nullptr;
			return true;
		}

		const bool succeeded =
			SDL_RenderGeometry(
				geometryBatch.renderer,
				geometryBatch.texture,
				geometryBatch.vertices.data(),
				static_cast<int>(geometryBatch.vertices.size()),
				geometryBatch.indices.data(),
				static_cast<int>(geometryBatch.indices.size())
			) == 0;

		geometryBatch.vertices.clear();
		geometryBatch.indices.clear();
		geometryBatch.renderer = nullptr;
		geometryBatch.texture = nullptr;
		geometryBatch.succeeded =
			geometryBatch.succeeded && succeeded;

		return succeeded;
	}

	bool submitGeometry(
		SDL_Renderer* renderer,
		SDL_Texture* texture,
		const SDL_Vertex* vertices,
		int vertexCount,
		const int* indices,
		int indexCount)
	{
		if (!geometryBatch.active) {
			return SDL_RenderGeometry(
				renderer,
				texture,
				vertices,
				vertexCount,
				indices,
				indexCount
			) == 0;
		}

		if (!geometryBatch.vertices.empty() &&
			(geometryBatch.renderer != renderer ||
				geometryBatch.texture != texture))
		{
			flushGeometryBatch();
		}

		geometryBatch.renderer = renderer;
		geometryBatch.texture = texture;

		const int vertexOffset =
			static_cast<int>(geometryBatch.vertices.size());

		geometryBatch.vertices.insert(
			geometryBatch.vertices.end(),
			vertices,
			vertices + vertexCount
		);

		geometryBatch.indices.reserve(
			geometryBatch.indices.size() +
			static_cast<std::size_t>(indexCount)
		);

		for (int i = 0; i < indexCount; ++i) {
			geometryBatch.indices.push_back(
				indices[i] + vertexOffset
			);
		}

		return geometryBatch.succeeded;
	}
}

void SDL::beginGeometryBatch(std::size_t expectedQuads) {
	if (geometryBatch.active) {
		flushGeometryBatch();
	}

	geometryBatch.active = true;
	geometryBatch.succeeded = true;
	geometryBatch.renderer = nullptr;
	geometryBatch.texture = nullptr;
	geometryBatch.vertices.clear();
	geometryBatch.indices.clear();

	const std::size_t expectedVertices = expectedQuads * 4;
	const std::size_t expectedIndices = expectedQuads * 6;

	if (geometryBatch.vertices.capacity() < expectedVertices) {
		geometryBatch.vertices.reserve(expectedVertices);
	}

	if (geometryBatch.indices.capacity() < expectedIndices) {
		geometryBatch.indices.reserve(expectedIndices);
	}
}

bool SDL::endGeometryBatch() {
	if (!geometryBatch.active) {
		return true;
	}

	flushGeometryBatch();
	geometryBatch.active = false;
	return geometryBatch.succeeded;
}

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
				// ensure vector sized once before the per-screen loop (or here; harmless)
				renderTargets_.resize(screenCount_, nullptr);

				// Create the SINGLE offscreen render target for compositing
				{
					SDL_Renderer* r = renderer_[logicalScreen];
					if (!r) return false;

					const int w = windowWidth_[logicalScreen];
					const int h = windowHeight_[logicalScreen];

					SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
						SDL_TEXTUREACCESS_TARGET, w, h);

					if (!t) {
						LOG_ERROR("SDL", "Failed to create render target texture: " + std::string(SDL_GetError()));
						return false;
					}

					// Use standard blend mode for compositing UI elements
					SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
					SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);

					// --- One-time init clear so contents are defined ---
					SDL_SetRenderTarget(r, t);
					SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
					SDL_RenderClear(r);
					SDL_SetRenderTarget(r, nullptr);

					renderTargets_[logicalScreen] = t;
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

			// During initialization.
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

void SDL::drawFitBars(
	int monitor,
	int layoutWidth,
	int layoutHeight) {
	// Stretch and Fill have no letterbox/pillarbox area.
	if (layoutScaleMode_ != LayoutScaleMode::Fit) {
		return;
	}

	if (monitor < 0 ||
		monitor >= screenCount_ ||
		!renderer_[monitor] ||
		layoutWidth <= 0 ||
		layoutHeight <= 0)
	{
		return;
	}

	/*
	 * Mirror mode uses its own historical scaling path in renderCopyF()
	 * and does not apply Fit/Fill scaling.
	 */
	if (mirror_[monitor]) {
		return;
	}

	SDL_Renderer* rr = renderer_[monitor];

	const int outW = windowWidth_[monitor];
	const int outH = windowHeight_[monitor];
	const int rot = rotation_[monitor] & 3;

	if (outW <= 0 || outH <= 0) {
		return;
	}

	/*
	 * Match renderCopyF():
	 *
	 * For 90/270-degree output rotation, scaling is calculated against
	 * a logical output whose width/height are swapped.
	 */
	const float logicalOutW =
		(rot & 1)
		? static_cast<float>(outH)
		: static_cast<float>(outW);

	const float logicalOutH =
		(rot & 1)
		? static_cast<float>(outW)
		: static_cast<float>(outH);

	const float scaleX =
		logicalOutW /
		static_cast<float>(layoutWidth);

	const float scaleY =
		logicalOutH /
		static_cast<float>(layoutHeight);

	const float scale =
		std::min(scaleX, scaleY);

	/*
	 * Size of the fitted virtual layout before output rotation.
	 */
	const float logicalViewportW =
		static_cast<float>(layoutWidth) * scale;

	const float logicalViewportH =
		static_cast<float>(layoutHeight) * scale;

	/*
	 * Convert fitted dimensions to final physical output orientation.
	 */
	const float viewportW =
		(rot & 1)
		? logicalViewportH
		: logicalViewportW;

	const float viewportH =
		(rot & 1)
		? logicalViewportW
		: logicalViewportH;

	/*
	 * Fit centers the virtual layout in the physical output.
	 */
	const float viewportX =
		(static_cast<float>(outW) - viewportW) * 0.5f;

	const float viewportY =
		(static_cast<float>(outH) - viewportH) * 0.5f;

	/*
	 * Round inward toward the visible viewport.
	 *
	 * This deliberately allows the black mask to cover at most a
	 * fractional edge pixel, preventing thin slivers of arbitrarily
	 * rotated geometry from leaking into the bars.
	 */
	const int left =
		std::clamp(
			static_cast<int>(std::ceil(viewportX)),
			0,
			outW
		);

	const int top =
		std::clamp(
			static_cast<int>(std::ceil(viewportY)),
			0,
			outH
		);

	const int right =
		std::clamp(
			static_cast<int>(
				std::floor(viewportX + viewportW)
				),
			0,
			outW
		);

	const int bottom =
		std::clamp(
			static_cast<int>(
				std::floor(viewportY + viewportH)
				),
			0,
			outH
		);

	SDL_Rect bars[4];
	int barCount = 0;

	// Top
	if (top > 0) {
		bars[barCount++] = {
			0,
			0,
			outW,
			top
		};
	}

	// Bottom
	if (bottom < outH) {
		bars[barCount++] = {
			0,
			bottom,
			outW,
			outH - bottom
		};
	}

	// Left
	if (left > 0 &&
		bottom > top)
	{
		bars[barCount++] = {
			0,
			top,
			left,
			bottom - top
		};
	}

	// Right
	if (right < outW &&
		bottom > top)
	{
		bars[barCount++] = {
			right,
			top,
			outW - right,
			bottom - top
		};
	}

	// Exact aspect-ratio match: nothing to mask.
	if (barCount == 0) {
		return;
	}

	SDL_SetRenderDrawColor(
		rr,
		0,
		0,
		0,
		255
	);

	/*
	 * Usually only two rectangles:
	 *
	 *   top + bottom
	 *        or
	 *   left + right
	 *
	 * One renderer call masks them all.
	 */
	SDL_RenderFillRects(
		rr,
		bars,
		barCount
	);
}

// Render a copy of a texture
bool SDL::renderCopy(SDL_Texture* texture, float alpha, SDL_Rect const* src, SDL_Rect const* dest, ViewInfo& viewInfo, int layoutWidth, int layoutHeight) {

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

	// ---------------------------------------------------------
	// Layout-to-output scale cache
	// ---------------------------------------------------------

	struct ScaleCache {
		int lastLW = -1;
		int lastLH = -1;
		int lastOW = -1;
		int lastOH = -1;
		int lastRot = -1;
		int lastMode = -1;

		bool lastMir = false;
		bool lastFS = false;

		float scaleX = 1.0f;
		float scaleY = 1.0f;

		// Output-pixel offset used by fit/fill.
		float offsetX = 0.0f;
		float offsetY = 0.0f;

		// Existing fullscreen-window offset, expressed in layout units.
		float dxL = 0.0f;
		float dyL = 0.0f;
	};

	static ScaleCache cache[8];

	// Avoid indexing past the fixed cache if screenCount_ is ever raised.
	if (m >= static_cast<int>(std::size(cache))) {
		LOG_ERROR(
			"SDL",
			"renderCopyF scale cache supports at most " +
			std::to_string(std::size(cache)) + " monitors"
		);
		return false;
	}

	const int outW = windowWidth_[m];
	const int outH = windowHeight_[m];
	const int rot = rotation_[m] & 3;
	const bool mir = mirror_[m];
	const bool fs = fullscreen_[m];
	const int modeKey = static_cast<int>(layoutScaleMode_);

	if (outW <= 0 || outH <= 0) {
		return true;
	}

	const bool cacheInvalid =
		cache[m].lastLW != layoutWidth ||
		cache[m].lastLH != layoutHeight ||
		cache[m].lastOW != outW ||
		cache[m].lastOH != outH ||
		cache[m].lastRot != rot ||
		cache[m].lastMir != mir ||
		cache[m].lastFS != fs ||
		cache[m].lastMode != modeKey;

	if (cacheInvalid) {
		/*
		 * Before output rotation is applied, 90/270-degree layouts render
		 * into an output coordinate system whose dimensions are swapped.
		 */
		const float logicalOutW =
			(rot & 1)
			? static_cast<float>(outH)
			: static_cast<float>(outW);

		const float logicalOutH =
			(rot & 1)
			? static_cast<float>(outW)
			: static_cast<float>(outH);

		const float candidateScaleX =
			logicalOutW / static_cast<float>(layoutWidth);

		const float candidateScaleY =
			logicalOutH / static_cast<float>(layoutHeight);

		cache[m].offsetX = 0.0f;
		cache[m].offsetY = 0.0f;

		/*
		 * Mirror mode historically renders the layout into half the output
		 * height and then duplicates/rotates it. Preserve that behavior.
		 *
		 * Normal non-mirrored rendering gets stretch, fit, or fill.
		 */
		if (mir) {
			cache[m].scaleX = candidateScaleX;
			cache[m].scaleY = candidateScaleY * 0.5f;
		}
		else {
			switch (layoutScaleMode_) {
				case LayoutScaleMode::Fit: {
					const float scale =
						std::min(candidateScaleX, candidateScaleY);

					cache[m].scaleX = scale;
					cache[m].scaleY = scale;

					cache[m].offsetX =
						(logicalOutW -
							static_cast<float>(layoutWidth) * scale) *
						0.5f;

					cache[m].offsetY =
						(logicalOutH -
							static_cast<float>(layoutHeight) * scale) *
						0.5f;

					break;
				}

				case LayoutScaleMode::Fill: {
					const float scale =
						std::max(candidateScaleX, candidateScaleY);

					cache[m].scaleX = scale;
					cache[m].scaleY = scale;

					cache[m].offsetX =
						(logicalOutW -
							static_cast<float>(layoutWidth) * scale) *
						0.5f;

					cache[m].offsetY =
						(logicalOutH -
							static_cast<float>(layoutHeight) * scale) *
						0.5f;

					break;
				}

				case LayoutScaleMode::Stretch:
				default:
				cache[m].scaleX = candidateScaleX;
				cache[m].scaleY = candidateScaleY;
				break;
			}
		}

		/*
		 * Existing fullscreen handling:
		 *
		 * dxL/dyL remain layout-space values because they are added to the
		 * logical destination rectangle before to_pixels() is called.
		 */
		cache[m].dxL = 0.0f;
		cache[m].dyL = 0.0f;

		if (fs) {
			cache[m].dxL =
				0.5f *
				static_cast<float>(displayWidth_[m] - outW) /
				std::max(cache[m].scaleX, 1e-6f);

			cache[m].dyL =
				0.5f *
				static_cast<float>(displayHeight_[m] - outH) /
				std::max(cache[m].scaleY, 1e-6f);
		}

		cache[m].lastLW = layoutWidth;
		cache[m].lastLH = layoutHeight;
		cache[m].lastOW = outW;
		cache[m].lastOH = outH;
		cache[m].lastRot = rot;
		cache[m].lastMir = mir;
		cache[m].lastFS = fs;
		cache[m].lastMode = modeKey;
	}

	const float scaleX = cache[m].scaleX;
	const float scaleY = cache[m].scaleY;

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

	// Texture color modulation is invariant for every quad emitted by
	// this renderCopyF() call, including reflections and mirror copies.
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

	// Existing fullscreen offset is in logical layout coordinates.
	dstRect.x += cache[m].dxL;
	dstRect.y += cache[m].dyL;

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

	if (mir && !hasContainer) {
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

	/*
	 * This is the key global layout viewport transform.
	 *
	 * Stretch:
	 *   offsets are zero, scaleX and scaleY differ.
	 *
	 * Fit:
	 *   scaleX == scaleY, offsets are positive.
	 *
	 * Fill:
	 *   scaleX == scaleY, one offset may be negative.
	 */
	auto to_pixels = [&](SDL_FRect rect) -> SDL_FRect {
		rect.x = cache[m].offsetX + rect.x * scaleX;
		rect.y = cache[m].offsetY + rect.y * scaleY;
		rect.w *= scaleX;
		rect.h *= scaleY;
		return rect;
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
	 * This stays entirely in layout space, so Fit/Fill scaling, output
	 * rotation, and SDL_RenderGeometry batching are unaffected.
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

	auto apply_output_rotation_rect =
		[&](SDL_FRect& rectPixels)
		{
			switch (rot) {
				case 1: {
					const float oldX = rectPixels.x;

					rectPixels.x =
						static_cast<float>(outW) -
						rectPixels.y -
						rectPixels.h * 0.5f -
						rectPixels.w * 0.5f;

					rectPixels.y =
						oldX -
						rectPixels.h * 0.5f +
						rectPixels.w * 0.5f;

					break;
				}

				case 2:
				rectPixels.x =
					static_cast<float>(outW) -
					rectPixels.x -
					rectPixels.w;

				rectPixels.y =
					static_cast<float>(outH) -
					rectPixels.y -
					rectPixels.h;

				break;

				case 3: {
					const float oldX = rectPixels.x;

					rectPixels.x =
						rectPixels.y +
						rectPixels.h * 0.5f -
						rectPixels.w * 0.5f;

					rectPixels.y =
						static_cast<float>(outH) -
						oldX -
						rectPixels.h * 0.5f -
						rectPixels.w * 0.5f;

					break;
				}

				default:
				break;
			}
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
						destinationPixels.w +
						std::fabs(sinAngle) *
						destinationPixels.h);

				const float halfExtentY =
					0.5f *
					(std::fabs(sinAngle) *
						destinationPixels.w +
						std::fabs(cosAngle) *
						destinationPixels.h);

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
						point.x - centerX;

					const float translatedY =
						point.y - centerY;

					point.x =
						translatedX * cosAngle -
						translatedY * sinAngle +
						centerX;

					point.y =
						translatedX * sinAngle +
						translatedY * cosAngle +
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

			return submitGeometry(
				renderer_[m],
				texture,
				vertices,
				4,
				indices,
				6
			);
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


			float angle = viewInfo.Angle;

			if (!mir) {
				angle += static_cast<float>(rot * 90);
			}

			SDL_FRect destinationPixels =
				to_pixels(destinationRect);

			bool result = true;

			if (mir) {
				if ((rotation_[m] & 1) == 0) {
					SDL_FRect mirroredRect =
						destinationPixels;

					mirroredRect.y +=
						static_cast<float>(outH) * 0.5f;

					result &= draw_quad(
						sourceRect,
						mirroredRect,
						angle,
						flipHorizontal,
						flipVertical,
						pathAlpha
					);

					mirroredRect.x =
						static_cast<float>(outW) -
						mirroredRect.x -
						mirroredRect.w;

					mirroredRect.y =
						static_cast<float>(outH) -
						mirroredRect.y -
						mirroredRect.h;

					result &= draw_quad(
						sourceRect,
						mirroredRect,
						angle + 180.0f,
						flipHorizontal,
						flipVertical,
						pathAlpha
					);
				}
				else {
					SDL_FRect mirroredRect =
						destinationPixels;

					const float oldX =
						mirroredRect.x;

					mirroredRect.x =
						static_cast<float>(outW) * 0.5f -
						mirroredRect.y -
						mirroredRect.h * 0.5f -
						mirroredRect.w * 0.5f;

					mirroredRect.y =
						oldX -
						mirroredRect.h * 0.5f +
						mirroredRect.w * 0.5f;

					result &= draw_quad(
						sourceRect,
						mirroredRect,
						angle + 90.0f,
						flipHorizontal,
						flipVertical,
						pathAlpha
					);

					mirroredRect.x =
						static_cast<float>(outW) -
						mirroredRect.x -
						mirroredRect.w;

					mirroredRect.y =
						static_cast<float>(outH) -
						mirroredRect.y -
						mirroredRect.h;

					result &= draw_quad(
						sourceRect,
						mirroredRect,
						angle + 270.0f,
						flipHorizontal,
						flipVertical,
						pathAlpha
					);
				}
			}
			else {
				apply_output_rotation_rect(
					destinationPixels
				);

				result &= draw_quad(
					sourceRect,
					destinationPixels,
					angle,
					flipHorizontal,
					flipVertical,
					pathAlpha
				);
			}

			return result;
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
