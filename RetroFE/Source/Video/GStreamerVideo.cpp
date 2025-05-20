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
#define NOMINMAX
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif
#include "GStreamerVideo.h"
#include "../Database/Configuration.h"
#include "../Graphics/Component/Image.h"
#include "../Graphics/ViewInfo.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gst/audio/audio.h>
#include <gst/gstdebugutils.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <array>
#include <mutex>
#include <algorithm>

bool GStreamerVideo::initialized_ = false;
bool GStreamerVideo::pluginsInitialized_ = false;

std::vector<GStreamerVideo*> GStreamerVideo::activeVideos_;
std::mutex GStreamerVideo::activeVideosMutex_;

// Initialize the static session ID generator
std::atomic<uint64_t> GStreamerVideo::nextUniquePlaySessionId_{ 1 };

typedef enum {
	GST_PLAY_FLAG_VIDEO = (1 << 0),
	GST_PLAY_FLAG_AUDIO = (1 << 1),
	GST_PLAY_FLAG_TEXT = (1 << 2),
	GST_PLAY_FLAG_VIS = (1 << 3),
	GST_PLAY_FLAG_SOFT_VOLUME = (1 << 4),
	GST_PLAY_FLAG_NATIVE_AUDIO = (1 << 5),
	GST_PLAY_FLAG_NATIVE_VIDEO = (1 << 6),
	GST_PLAY_FLAG_DOWNLOAD = (1 << 7),
	GST_PLAY_FLAG_BUFFERING = (1 << 8),
	GST_PLAY_FLAG_DEINTERLACE = (1 << 9),
	GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
	GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
	GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

static const SDL_BlendMode softOverlayBlendMode = SDL_ComposeCustomBlendMode(
	SDL_BLENDFACTOR_SRC_ALPHA,           // Source color factor: modulates source color by the alpha value set dynamically
	SDL_BLENDFACTOR_ONE,                 // Destination color factor: keep the destination as is
	SDL_BLENDOPERATION_ADD,              // Color operation: add source and destination colors based on alpha
	SDL_BLENDFACTOR_ONE,                 // Source alpha factor
	SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, // Destination alpha factor: inverse of source alpha
	SDL_BLENDOPERATION_ADD               // Alpha operation: add alpha values
);

struct Point2D {
	double x;
	double y;
	constexpr Point2D(double x, double y) : x(x), y(y) {}
};

#ifdef WIN32
static bool IsIntelGPU() {
	Microsoft::WRL::ComPtr<IDXGIFactory> factory;
	if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf())))) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		// Check if the vendor ID matches Intel's vendor ID
		if (desc.VendorId == 0x8086) { // 0x8086 is the vendor ID for Intel
			return true;
		}
	}

	return false;
}
#endif

GStreamerVideo::GStreamerVideo(int monitor)

	: monitor_(monitor)

{
	initialize();
	initializePlugins();
	createAlphaTexture();
}


GStreamerVideo::~GStreamerVideo() {
	stop();
}

void GStreamerVideo::createAlphaTexture() {
	SDL_LockMutex(SDL::getMutex()); // Per-monitor mutex
	if (alphaTexture_) {
		SDL_UnlockMutex(SDL::getMutex());
		return;
	}

	alphaTexture_ = SDL_CreateTexture(
		SDL::getRenderer(monitor_), SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, ALPHA_TEXTURE_SIZE, ALPHA_TEXTURE_SIZE);

	if (!alphaTexture_) {
		LOG_ERROR("GStreamerVideo", "Failed to create alpha texture: " + std::string(SDL_GetError()));
		hasError_.store(true, std::memory_order_release);
		SDL_UnlockMutex(SDL::getMutex());
		return;
	}

	SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;
	SDL_SetTextureBlendMode(alphaTexture_, blendMode);

	const int pitch = ALPHA_TEXTURE_SIZE * 4;
	const int size = pitch * ALPHA_TEXTURE_SIZE;
	std::vector<uint8_t> pixels(size, 0);
	if (SDL_UpdateTexture(alphaTexture_, nullptr, pixels.data(), pitch) != 0) {
		LOG_ERROR("GStreamerVideo", "Failed to update alpha texture: " + std::string(SDL_GetError()));
		hasError_.store(true, std::memory_order_release);
	}
	texture_ = alphaTexture_;
	SDL_UnlockMutex(SDL::getMutex());
}

// In GStreamerVideo.cpp

gboolean GStreamerVideo::busCallback(GstBus * bus, GstMessage * msg, gpointer user_data) {
	GStreamerVideo* video = static_cast<GStreamerVideo*>(user_data);

	// It's good practice to ensure 'video' is valid, especially in callbacks.
	if (!video) {
		LOG_ERROR("GStreamerVideo::busCallback", "Callback invoked with null user_data.");
		// If user_data is null, we can't operate.
		// Returning FALSE would remove the watch, which might be desired if this is a critical error.
		// However, if the bus watch was added with g_source_remove_on_dispatch, it might be removed anyway.
		// For safety, let's just log and continue, though this state indicates a deeper problem.
		return TRUE;
	}
	// Optional: Check if playbin_ is still valid if there's any chance it could be destroyed
	// while a bus message is pending.
	if (!video->playbin_) {
		LOG_WARNING("GStreamerVideo::busCallback", "Callback invoked but playbin_ is null for file: " + video->currentFile_);
		return TRUE; // Continue, but state is unusual.
	}


	switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_STATE_CHANGED: {
			// Ensure the state change is from our playbin element
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->playbin_)) {
				GstState oldState, newState, pending;
				gst_message_parse_state_changed(msg, &oldState, &newState, &pending);

				video->paused_.store(newState == GST_STATE_PAUSED, std::memory_order_release);

				if (newState == GST_STATE_PLAYING) {
					video->targetState_ = IVideo::VideoState::Playing;
				}
				else if (newState == GST_STATE_PAUSED) {
					video->targetState_ = IVideo::VideoState::Paused;
				}
				else if (newState == GST_STATE_NULL || newState == GST_STATE_READY) {
					// If pipeline goes to NULL or READY unexpectedly (not via explicit stop/unload),
					// it might indicate an issue or that it has finished/failed.
					// isPlaying_ should reflect this.
					if (video->isPlaying_.load(std::memory_order_acquire) && (newState == GST_STATE_NULL || (newState == GST_STATE_READY && oldState != GST_STATE_PAUSED && oldState != GST_STATE_PLAYING))) {
						LOG_WARNING("GStreamerVideo", "BusCallback: Pipeline for " + video->currentFile_ +
							" transitioned to " + std::string(gst_element_state_get_name(newState)) +
							" unexpectedly from " + std::string(gst_element_state_get_name(oldState)) +
							". Playback may have stopped.");
						// Consider setting hasError_ or isPlaying_ = false if this is unexpected
					}
					video->targetState_ = IVideo::VideoState::None;
				}


				LOG_DEBUG("GStreamerVideo", "BusCallback: State changed: " +
					std::string(gst_element_state_get_name(oldState)) + " -> " +
					std::string(gst_element_state_get_name(newState)) +
					(pending == GST_STATE_VOID_PENDING ? "" : " (pending: " + std::string(gst_element_state_get_name(pending)) + ")") +
					" for " + video->currentFile_ + " (Session: " + std::to_string(video->currentPlaySessionId_.load()) + ")");
			}
			break;
		}
		case GST_MESSAGE_EOS: {
			// Check if the EOS is from our main playbin element
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->playbin_)) {
				LOG_DEBUG("GStreamerVideo", "BusCallback: Received EOS for " + video->currentFile_ +
					" (Session: " + std::to_string(video->currentPlaySessionId_.load()) + ")");
				if (video->playbin_ && video->getCurrent() > GST_SECOND / 2) { // Check if it played for a meaningful duration
					video->playCount_++;
					if (!video->numLoops_ || video->numLoops_ > video->playCount_) {
						LOG_DEBUG("GStreamerVideo", "BusCallback: Looping " + video->currentFile_);
						video->restart();
					}
					else {
						LOG_DEBUG("GStreamerVideo", "BusCallback: Finished loops for " + video->currentFile_ + ". Pausing.");
						video->pause(); // Or perhaps a full stop/unload depending on desired behavior
						// video->isPlaying_.store(false, std::memory_order_release); // If EOS means it's no longer "playing"
					}
				}
				else if (video->playbin_) {
					LOG_DEBUG("GStreamerVideo", "BusCallback: EOS received very early for " + video->currentFile_ + ". Not looping.");
					// Potentially an issue if EOS comes too fast, could indicate a problem loading the file.
					// video->hasError_.store(true, std::memory_order_release);
					// video->isPlaying_.store(false, std::memory_order_release);
				}
			}
			break;
		}
		case GST_MESSAGE_ERROR: {
			GError* err = nullptr;
			gchar* dbg_info = nullptr;

			gst_message_parse_error(msg, &err, &dbg_info);

			LOG_ERROR("GStreamerVideo", "BusCallback: GStreamer ERROR received for " + video->currentFile_ +
				" (Session: " + std::to_string(video->currentPlaySessionId_.load()) + "): " +
				(err ? err->message : "Unknown error") +
				(dbg_info ? (std::string(" | Debug: ") + dbg_info) : ""));

			video->hasError_.store(true, std::memory_order_release);
			video->isPlaying_.store(false, std::memory_order_release); // Typically an error means playback stops
			video->targetState_ = IVideo::VideoState::None; // Reflect that it's no longer in a valid playing/paused state

			if (err) g_error_free(err);
			if (dbg_info) g_free(dbg_info);
			break;
		}
							  // LOG_DEBUG("GStreamerVideo", "BusCallback: Unhandled message type: " + std::to_string(GST_MESSAGE_TYPE(msg)));
							  break;
	}

	return TRUE; // Keep the bus watch active
}
void GStreamerVideo::initializePlugins() {
	if (!pluginsInitialized_)
	{
		pluginsInitialized_ = true;

#if defined(WIN32)
		//enablePlugin("directsoundsink");
		disablePlugin("mfdeviceprovider");
		disablePlugin("nvh264dec");
		disablePlugin("nvh265dec");
		if (Configuration::HardwareVideoAccel)
		{
			if (IsIntelGPU())
			{
				enablePlugin("qsvh264dec");
				enablePlugin("qsvh265dec");
				disablePlugin("d3d11h264dec");
				disablePlugin("d3d11h265dec");
				disablePlugin("d3d12h264dec");
				disablePlugin("d3d12h265dec");

				LOG_DEBUG("GStreamerVideo", "Using qsvh264dec/qsvh265dec for Intel GPU");
			}
			else
			{
				enablePlugin("d3d12h264dec");
				enablePlugin("d3d12h265dec");
				disablePlugin("qsvh264dec");
				disablePlugin("qsvh265dec");

				LOG_DEBUG("GStreamerVideo", "Using d3d11h264dec/d3d11h265dec for non-Intel GPU");
			}
		}
		else
		{
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
			disablePlugin("d3d11h264dec");
			disablePlugin("d3d11h265dec");
			disablePlugin("d3d12h264dec");
			disablePlugin("d3d12h265dec");
			disablePlugin("qsvh264dec");
			disablePlugin("qsvh265dec");
			LOG_DEBUG("GStreamerVideo", "Using avdec_h264/avdec_h265 for software decoding");
		}
#elif defined(__APPLE__)
		if (!Configuration::HardwareVideoAccel) {
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
			LOG_DEBUG("GStreamerVideo", "Using avdec_h264/avdec_h265 for software decoding");
		}
#else
		//enablePlugin("pipewiresink");
		//disablePlugin("alsasink");
		//disablePlugin("pulsesink");
		if (Configuration::HardwareVideoAccel)
		{
			enablePlugin("vah264dec");
			enablePlugin("vah265dec");
		}
		if (!Configuration::HardwareVideoAccel)
		{
			disablePlugin("vah264dec");
			disablePlugin("vah265dec");
			//enablePlugin("openh264dec");
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
		}
#endif
	}
}

void GStreamerVideo::setNumLoops(int n) {
	if (n > 0)
		numLoops_ = n;
}

SDL_Texture* GStreamerVideo::getTexture() const {
	SDL_LockMutex(SDL::getMutex());
	SDL_Texture* currentTexture = texture_;
	SDL_UnlockMutex(SDL::getMutex());
	return currentTexture;
}

bool GStreamerVideo::initialize() {
	if (initialized_) return true;
	if (!gst_is_initialized())
	{
		LOG_DEBUG("GStreamer", "Initializing in instance");
		gst_init(nullptr, nullptr);
		std::string path = Utils::combinePath(Configuration::absolutePath, "retrofe");
#ifdef WIN32
		//GstRegistry* registry = gst_registry_get();
		//gst_registry_scan_path(registry, path.c_str());
#endif
	}
	gst_debug_add_log_function(customGstLogHandler, nullptr, nullptr);
	initialized_ = true;
	return true;
}

bool GStreamerVideo::deInitialize() {
	gst_deinit();
	initialized_ = false;
	return true;
}

bool GStreamerVideo::stop() {
	if (!initialized_)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(drawMutex_);

	LOG_DEBUG("GStreamerVideo", "Stopping video: " + currentFile_ + " for session ID: " + std::to_string(currentPlaySessionId_.load()));
	isPlaying_.store(false, std::memory_order_release);

	// --- Proactively remove any active pad probe ---
	if (padProbeId_ != 0 && playbin_ && videoSink_) { // Check playbin_ as well, as it might be set to NULL path
		GstPad* sinkPad = gst_element_get_static_pad(GST_ELEMENT(videoSink_), "sink");
		if (sinkPad) {
			LOG_DEBUG("GStreamerVideo", "Stop: Removing active pad probe ID " + std::to_string(padProbeId_) + " for " + currentFile_);
			gst_pad_remove_probe(sinkPad, padProbeId_);
			gst_object_unref(sinkPad);
		}
		else {
			LOG_WARNING("GStreamerVideo", "Stop: Could not get sink pad to remove probe for " + currentFile_);
		}
		padProbeId_ = 0; // Mark as no active probe from our side
	}

	if (playbin_)
	{
		if (busWatchId_ != 0) {
			g_source_remove(busWatchId_);
			busWatchId_ = 0;
		}

		// Set the pipeline state to NULL
		gst_element_set_state(playbin_, GST_STATE_NULL);

		// Wait for the state change to complete
		GstState state;
		GstStateChangeReturn ret = gst_element_get_state(playbin_, &state, nullptr, GST_SECOND);
		if (ret != GST_STATE_CHANGE_SUCCESS)
		{
			LOG_ERROR("Video", "Failed to change playbin state to NULL");
		}

		// Disconnect signal handlers
		if (elementSetupHandlerId_)
		{
			g_signal_handler_disconnect(playbin_, elementSetupHandlerId_);
			elementSetupHandlerId_ = 0;
		}

		gst_object_unref(playbin_);

		playbin_ = nullptr;
		videoSink_ = nullptr;
	}

	SDL_LockMutex(SDL::getMutex());

	if (texture_ != nullptr)
	{
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	if (videoTexture_ != nullptr)
	{
		SDL_DestroyTexture(videoTexture_);
		videoTexture_ = nullptr;
	}
	if (alphaTexture_ != nullptr)
	{
		SDL_DestroyTexture(alphaTexture_);
		alphaTexture_ = nullptr;
	}
	SDL_UnlockMutex(SDL::getMutex());
	if (perspective_gva_) {
		g_value_array_free(perspective_gva_);
		perspective_gva_ = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(activeVideosMutex_);
		activeVideos_.erase(std::remove(activeVideos_.begin(), activeVideos_.end(), this), activeVideos_.end());
	}
	return true;
}

bool GStreamerVideo::unload() {
	if (!playbin_) {
		return false;
	}

	std::lock_guard<std::mutex> lock(drawMutex_);

	LOG_DEBUG("GStreamerVideo", "Unloading video: " + currentFile_ + " for session ID: " + std::to_string(currentPlaySessionId_.load()));

	isPlaying_.store(false, std::memory_order_release);

	// --- Proactively remove any active pad probe ---
	if (padProbeId_ != 0 && videoSink_) {
		GstPad* sinkPad = gst_element_get_static_pad(GST_ELEMENT(videoSink_), "sink");
		if (sinkPad) {
			LOG_DEBUG("GStreamerVideo", "Unload: Removing active pad probe ID " + std::to_string(padProbeId_) + " for " + currentFile_);
			gst_pad_remove_probe(sinkPad, padProbeId_);
			gst_object_unref(sinkPad);
		}
		else {
			LOG_WARNING("GStreamerVideo", "Unload: Could not get sink pad to remove probe for " + currentFile_);
		}
		padProbeId_ = 0; // Mark as no active probe from our side
	}

	if (busWatchId_ != 0) {
		g_source_remove(busWatchId_);
		busWatchId_ = 0;
	}

	// 1. Check current and pending state
	GstState curState, pendingState;
	GstStateChangeReturn getStateRet = gst_element_get_state(playbin_, &curState, &pendingState, GST_SECOND);

	bool needsPause = true;

	if (getStateRet != GST_STATE_CHANGE_FAILURE) {
		if (curState == GST_STATE_PAUSED || pendingState == GST_STATE_PAUSED) {
			needsPause = false;
		}
	}

	// 2. Gracefully pause if necessary
	if (needsPause) {
		gst_element_set_state(playbin_, GST_STATE_PAUSED);
		// Now block briefly (not forever) to allow pause to complete
		gst_element_get_state(playbin_, nullptr, nullptr, GST_SECOND);
	}

	// 4. Move pipeline to READY
	GstStateChangeReturn setRet = gst_element_set_state(playbin_, GST_STATE_READY);
	GstState newState;
	GstStateChangeReturn getRet = gst_element_get_state(playbin_, &newState, nullptr, 200 * GST_MSECOND); // Shorter timeout

	if (setRet == GST_STATE_CHANGE_FAILURE || getRet == GST_STATE_CHANGE_FAILURE || (getRet != GST_STATE_CHANGE_ASYNC && newState != GST_STATE_READY)) {
		LOG_ERROR("GStreamerVideo", "Pipeline failed to reach READY state during unload for " + currentFile_ + ". Marking as error.");
		hasError_.store(true, std::memory_order_release);
		// Don't return false immediately, let other cleanup happen, error is now flagged.
	}

	// 5. Clean up bus
	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
	if (bus) {
		gst_bus_set_flushing(bus, TRUE);
		gst_object_unref(bus);
	}

	// 6. Reset everything else (same as before)
	targetState_ = IVideo::VideoState::None;
	currentFile_.clear();
	playCount_ = 0;
	numLoops_ = 0;
	currentVolume_ = 0.0f;
	lastSetVolume_ = -1.0f;
	lastSetMuteState_ = false;
	volume_ = 0.0f;

	mappingGeneration_.store(0, std::memory_order_release); // Reset for the next playback session

	textureWidth_.store(width_.load(std::memory_order_acquire), std::memory_order_release);
	textureHeight_.store(height_.load(std::memory_order_acquire), std::memory_order_release);
	width_.store(0, std::memory_order_release);
	height_.store(0, std::memory_order_release);

	hasValidFrame_.store(false, std::memory_order_release);

	SDL_LockMutex(SDL::getMutex());
	texture_ = alphaTexture_;  // fallback to blank
	textureValid_.store(false, std::memory_order_release);
	SDL_UnlockMutex(SDL::getMutex());

	LOG_DEBUG("GStreamerVideo", "Pipeline and class fully unloaded, ready for new play().");

	return true;
}

// Main function to compute perspective transform from 4 arbitrary points
static inline std::array<double, 9> computePerspectiveMatrixFromCorners(
	int width,
	int height,
	const std::array<Point2D, 4>&pts) {
	constexpr double EPSILON = 1e-9;

	const Point2D A = pts[0];
	const Point2D B = pts[1];
	const Point2D D = pts[2];
	const Point2D C = pts[3];


	double M11 = B.x - C.x;
	double M12 = D.x - C.x;
	double M21 = B.y - C.y;
	double M22 = D.y - C.y;
	double RHS1 = A.x - C.x;
	double RHS2 = A.y - C.y;

	double denom = M11 * M22 - M12 * M21;
	if (std::abs(denom) < EPSILON)
		return { 1,0,0, 0,1,0, 0,0,1 };

	double X = (RHS1 * M22 - RHS2 * M12) / denom; // X = g+1
	double Y = (M11 * RHS2 - M21 * RHS1) / denom; // Y = h+1

	double g = X - 1.0;
	double h = Y - 1.0;

	// Now compute the remaining coefficients using the (1,0) and (0,1) constraints:
	double a = X * B.x - A.x;  // a = (g+1)*B.x - A.x
	double d = X * B.y - A.y;
	double b = Y * D.x - A.x;  // b = (h+1)*D.x - A.x
	double e = Y * D.y - A.y;
	double c = A.x;
	double f = A.y;

	// Construct the forward homography Hf:
	// Hf maps (u,v) in [0,1]² to the quadrilateral.
	std::array<double, 9> Hf = { a, b, c,
								 d, e, f,
								 g, h, 1.0 };

	// --- Invert Hf to obtain H, which maps the quadrilateral to the unit square ---
	double det = Hf[0] * (Hf[4] * Hf[8] - Hf[5] * Hf[7])
		- Hf[1] * (Hf[3] * Hf[8] - Hf[5] * Hf[6])
		+ Hf[2] * (Hf[3] * Hf[7] - Hf[4] * Hf[6]);
	if (std::abs(det) < EPSILON)
		return { 1,0,0, 0,1,0, 0,0,1 };
	double invDet = 1.0 / det;
	std::array<double, 9> H = {
		(Hf[4] * Hf[8] - Hf[5] * Hf[7]) * invDet,
		(Hf[2] * Hf[7] - Hf[1] * Hf[8]) * invDet,
		(Hf[1] * Hf[5] - Hf[2] * Hf[4]) * invDet,
		(Hf[5] * Hf[6] - Hf[3] * Hf[8]) * invDet,
		(Hf[0] * Hf[8] - Hf[2] * Hf[6]) * invDet,
		(Hf[2] * Hf[3] - Hf[0] * Hf[5]) * invDet,
		(Hf[3] * Hf[7] - Hf[4] * Hf[6]) * invDet,
		(Hf[1] * Hf[6] - Hf[0] * Hf[7]) * invDet,
		(Hf[0] * Hf[4] - Hf[1] * Hf[3]) * invDet
	};

	// Normalize so that H[8] == 1.
	for (double& val : H) {
		val /= H[8];
	}
	// --- Scale the first two rows by the output dimensions ---
	H[0] *= width; H[1] *= width; H[2] *= width;
	H[3] *= height; H[4] *= height; H[5] *= height;

	return H;
}

bool GStreamerVideo::createPipelineIfNeeded() {
	if (playbin_) {
		return true;
	}

	// Create playbin3 and appsink elements.
	playbin_ = gst_element_factory_make("playbin3", "player");
	videoSink_ = gst_element_factory_make("appsink", "video_sink");

	if (!playbin_ || !videoSink_) {
		LOG_DEBUG("Video", "Could not create GStreamer elements");
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	// Set playbin flags and properties.
	gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_SOFT_VOLUME;
	g_object_set(playbin_, "flags", flags, "instant-uri", TRUE, "async-handling", FALSE, nullptr);

	// Configure appsink.
	g_object_set(videoSink_,
		"max-buffers", 1,  // Only keep latest buffer.
		"drop", TRUE,      // Drop old buffers.
		"wait-on-eos", FALSE,
		nullptr);

	// Set caps depending on whether perspective is enabled.
	GstCaps* videoCaps = nullptr;
	if (hasPerspective_) {
		// Enforce RGBA since the perspective element only accepts RGBA.
		videoCaps = gst_caps_from_string(
			"video/x-raw,format=(string)RGBA,pixel-aspect-ratio=(fraction)1/1");
		sdlFormat_ = SDL_PIXELFORMAT_ABGR8888;
		LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_ABGR8888 (Perspective enabled)");
	}
	else {
		if (Configuration::HardwareVideoAccel) {
			videoCaps = gst_caps_from_string(
				"video/x-raw,format=(string)NV12,pixel-aspect-ratio=(fraction)1/1");
			sdlFormat_ = SDL_PIXELFORMAT_NV12;
			LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_NV12 (HW accel: true)");
		}
		else {
			videoCaps = gst_caps_from_string(
				"video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1");
			elementSetupHandlerId_ = g_signal_connect(playbin_, "element-setup",
				G_CALLBACK(elementSetupCallback), this);
			sdlFormat_ = SDL_PIXELFORMAT_IYUV;
			LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_IYUV (HW accel: false)");
		}
	}
	gst_app_sink_set_caps(GST_APP_SINK(videoSink_), videoCaps);
	gst_caps_unref(videoCaps);

	if (hasPerspective_) {
		// Create and set up the perspective pipeline.
		perspective_ = gst_element_factory_make("perspective", "perspective");
		if (!perspective_) {
			LOG_DEBUG("GStreamerVideo", "Could not create perspective element");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		GstElement* videoBin = gst_bin_new("video_bin");
		if (!videoBin) {
			LOG_DEBUG("GStreamerVideo", "Could not create video bin");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		gst_bin_add_many(GST_BIN(videoBin), perspective_, videoSink_, nullptr);

		// Link perspective to appsink.
		if (!gst_element_link(perspective_, videoSink_)) {
			LOG_DEBUG("GStreamerVideo", "Could not link perspective to appsink");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		// Create a ghost pad to expose the sink pad of the perspective element.
		GstPad* perspectiveSinkPad = gst_element_get_static_pad(perspective_, "sink");
		if (!perspectiveSinkPad) {
			LOG_DEBUG("GStreamerVideo", "Could not get sink pad from perspective element");
			hasError_.store(true, std::memory_order_release);
			return false;
		}
		GstPad* ghostPad = gst_ghost_pad_new("sink", perspectiveSinkPad);
		gst_object_unref(perspectiveSinkPad);
		if (!gst_element_add_pad(videoBin, ghostPad)) {
			LOG_DEBUG("GStreamerVideo", "Could not add ghost pad to video bin");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		// Set the bin as the video-sink.
		g_object_set(playbin_, "video-sink", videoBin, nullptr);
	}
	else {
		// Simple pipeline: set appsink directly as video-sink.
		g_object_set(playbin_, "video-sink", videoSink_, nullptr);
	}

	{
		std::lock_guard<std::mutex> lock(activeVideosMutex_);
		activeVideos_.push_back(this);
	}

	initializeUpdateFunction();

	return true;
}

void GStreamerVideo::initializeUpdateFunction() {
	switch (sdlFormat_) {
		case SDL_PIXELFORMAT_IYUV:
		updateTextureFunc_ = [this](SDL_Texture* tex, GstVideoFrame* frame) {
			return updateTextureFromFrameIYUV(tex, frame);
			};
		break;
		case SDL_PIXELFORMAT_NV12:
		updateTextureFunc_ = [this](SDL_Texture* tex, GstVideoFrame* frame) {
			return updateTextureFromFrameNV12(tex, frame);
			};
		break;
		case SDL_PIXELFORMAT_ABGR8888:
		updateTextureFunc_ = [this](SDL_Texture* tex, GstVideoFrame* frame) {
			return updateTextureFromFrameRGBA(tex, frame);
			};
		break;
		default:
		LOG_ERROR("GStreamerVideo", "Unsupported pixel format during initialization");
		updateTextureFunc_ = [](SDL_Texture*, GstVideoFrame*) {
			return false;
			};
		break;
	}
}

bool GStreamerVideo::play(const std::string & file) {
	playCount_ = 0;
	if (!initialized_) {
		LOG_ERROR("GStreamerVideo", "Play called but GStreamer not initialized for file: " + file);
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	// Atomically increment and assign a new unique session ID for this play attempt.
	// This ID is unique across all GStreamerVideo instances and all play attempts.
	currentPlaySessionId_.store(nextUniquePlaySessionId_++, std::memory_order_release);

	// 1. Create the pipeline if we haven’t already
	if (!createPipelineIfNeeded()) {
		LOG_ERROR("Video", "Failed to create GStreamer pipeline");
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	// Reconnect the pad probe if it's not connected.
	// This should be done for each new 'play' call to ensure the probe is fresh for this stream.
	GstPad* sinkPad = gst_element_get_static_pad(videoSink_, "sink");
	if (sinkPad) {
		if (padProbeId_ != 0) { // Remove any existing probe on this pad from this instance
			gst_pad_remove_probe(sinkPad, padProbeId_);
			padProbeId_ = 0;
		}

		// --- Create Userdata for the Probe ---
		PadProbeUserdata* userdataForProbe = static_cast<PadProbeUserdata*>(g_malloc(sizeof(PadProbeUserdata)));
		if (!userdataForProbe) {
			LOG_ERROR("GStreamerVideo", "Failed to allocate memory for PadProbeUserdata for file: " + file);
			gst_object_unref(sinkPad);
			hasError_.store(true);
			return false;
		}
		userdataForProbe->videoInstance = this;
		userdataForProbe->playSessionId = currentPlaySessionId_.load(std::memory_order_acquire);

		padProbeId_ = gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
			GStreamerVideo::padProbeCallback,
			userdataForProbe,
			g_free); // now matches allocation

		gst_object_unref(sinkPad);
	}
	else {
		LOG_ERROR("GStreamerVideo", "Failed to get sink pad from videoSink_ for file: " + file);
		hasError_.store(true);
		return false;
	}

	GstBus* bus = gst_element_get_bus(playbin_);
	busWatchId_ = gst_bus_add_watch(bus, GStreamerVideo::busCallback, this);
	gst_object_unref(bus);

	// Convert file path to URI
	gchar* uriFile = gst_filename_to_uri(file.c_str(), nullptr);
	if (!uriFile) {
		LOG_DEBUG("Video", "Failed to convert filename to URI");
		hasError_.store(true, std::memory_order_release);
		return false;
	}


	// Update URI - no need to set to READY first
	g_object_set(playbin_, "uri", uriFile, nullptr);
	g_free(uriFile);

	
	GstState current, pending;
	gst_element_get_state(playbin_, &current, &pending, GST_SECOND);

	if (current != GST_STATE_PAUSED) {
		GstStateChangeReturn stateRet = gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PAUSED);
		if (stateRet != GST_STATE_CHANGE_ASYNC && stateRet != GST_STATE_CHANGE_SUCCESS) {
			isPlaying_.store(false, std::memory_order_release);
			hasError_.store(true, std::memory_order_release);
			return false;
		}
	}
	targetState_ = IVideo::VideoState::Paused;  // accurately reflect that we're starting preloaded
	paused_.store(true, std::memory_order_release);  // draw() must treat this as preloaded-not-playing
	isPlaying_.store(true, std::memory_order_release);
	currentFile_ = file;

	// Mute and volume to 0 by default
	gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_), GST_STREAM_VOLUME_FORMAT_LINEAR, 0.0);
	gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), true);
	lastSetMuteState_ = true;

	// Optionally wait for PLAYING state if you want to confirm it's active
	if (Configuration::debugDotEnabled)
	{
		// Environment variable is set, proceed with dot file generation
		GstState dotDebugState;
		GstState dotDebugPending;
		// Wait up to 5 seconds for the state change to complete
		GstClockTime timeout = 5 * GST_SECOND; // Define your timeout
		GstStateChangeReturn ret = gst_element_get_state(GST_ELEMENT(playbin_), &dotDebugState, &dotDebugPending, timeout);
		if (ret == GST_STATE_CHANGE_SUCCESS && dotDebugState == GST_STATE_PLAYING)
		{
			// The pipeline is in the playing state, proceed with dot file generation
			// Generate dot file for playbin_
			std::string playbinDotFileName = generateDotFileName("playbin", currentFile_);
			GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(playbin_), GST_DEBUG_GRAPH_SHOW_ALL, playbinDotFileName.c_str());
		}
	}

	LOG_DEBUG("GStreamerVideo", "Loaded " + file);

	return true;
}

void GStreamerVideo::elementSetupCallback([[maybe_unused]] GstElement * playbin, GstElement * element, [[maybe_unused]] gpointer data) {
	// Check if the element is a video decoder
	if (!Configuration::HardwareVideoAccel && GST_IS_VIDEO_DECODER(element))
	{
		// Configure the video decoder
		g_object_set(element, "thread-type", Configuration::AvdecThreadType,
			"max-threads", Configuration::AvdecMaxThreads,
			"direct-rendering", FALSE, "std-compliance", 0, nullptr);
	}
}

GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad * pad, GstPadProbeInfo * info, gpointer user_data) {
	PadProbeUserdata* probeData = static_cast<PadProbeUserdata*>(user_data);
	// It's crucial that 'user_data' (PadProbeUserdata) is valid until the probe is removed.
	// g_free as GDestroyNotify handles its deallocation.

	if (!probeData || !probeData->videoInstance) {
		LOG_WARNING("GStreamerVideo::padProbeCallback", "Probe fired with invalid user_data or videoInstance.");
		// If probeData itself is null, we can't even get videoInstance to check session ID.
		// This indicates a severe issue, possibly with probe setup or memory corruption.
		return GST_PAD_PROBE_REMOVE; // Remove the probe to prevent further issues.
	}

	GStreamerVideo* video = probeData->videoInstance;
	uint64_t callbackSessionId = probeData->playSessionId;

	// CRITICAL CHECK 1: Is this probe for the CURRENT playback session of this GStreamerVideo instance?
	uint64_t videoCurrentSessionId = video->currentPlaySessionId_.load(std::memory_order_acquire);
	if (videoCurrentSessionId != callbackSessionId) {
		LOG_DEBUG("GStreamerVideo::padProbeCallback", "Stale probe for session ID: " + std::to_string(callbackSessionId) +
			" (current video session ID: " + std::to_string(videoCurrentSessionId) +
			" for file: " + video->currentFile_ + "). Ignoring.");
		return GST_PAD_PROBE_REMOVE; // This probe is for an old, abandoned play attempt.
	}

	// CRITICAL CHECK 2: Is the pipeline (playbin_) still associated with this instance and active?
	// This helps if stop() was called (which nullifies playbin_) or if the instance is otherwise invalid.
	if (!video->playbin_ || !video->isPlaying_.load(std::memory_order_acquire)) {
		LOG_DEBUG("GStreamerVideo::padProbeCallback", "Probe for session ID: " + std::to_string(callbackSessionId) +
			" fired, but playbin_ is NULL or video is not marked as playing (file: " + video->currentFile_ + "). Instance likely stopped/reset. Ignoring.");
		return GST_PAD_PROBE_REMOVE;
	}

	auto* event = GST_PAD_PROBE_INFO_EVENT(info);
	if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
		GstCaps* caps = nullptr;
		gst_event_parse_caps(event, &caps);
		if (caps) {
			const GstStructure* s = gst_caps_get_structure(caps, 0);
			int newWidth = 0, newHeight = 0;

			if (gst_structure_get_int(s, "width", &newWidth) &&
				gst_structure_get_int(s, "height", &newHeight) &&
				newWidth > 0 && newHeight > 0) {

				video->width_.store(newWidth, std::memory_order_release);
				video->height_.store(newHeight, std::memory_order_release);

				bool dimensionsChangedOrTextureMissing;
				SDL_LockMutex(SDL::getMutex()); // Protects videoTexture_ and textureWidth_/Height_ (which describe videoTexture_)

				// textureWidth_ and textureHeight_ store the dimensions of the *current SDL videoTexture_*
				if (video->videoTexture_ &&
					video->textureWidth_.load(std::memory_order_acquire) == newWidth &&
					video->textureHeight_.load(std::memory_order_acquire) == newHeight) {

					dimensionsChangedOrTextureMissing = false;
					video->textureValid_.store(true, std::memory_order_release); // Current texture is valid for new dimensions
					LOG_DEBUG("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) + "): Reusing existing texture for " + video->currentFile_ +
						" (" + std::to_string(newWidth) + "x" + std::to_string(newHeight) + ")");
				}
				else {
					dimensionsChangedOrTextureMissing = true;
					video->textureValid_.store(false, std::memory_order_release); // Mark as invalid, new texture needed
					if (video->texture_ != video->alphaTexture_) { // Avoid redundant assignment
						LOG_DEBUG("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) + "): Switching to alphaTexture_ for " + video->currentFile_);
						video->texture_ = video->alphaTexture_; // Immediately fallback to blank
					}
					LOG_DEBUG("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) + "): New texture needed for " + video->currentFile_ +
						" (" + std::to_string(newWidth) + "x" + std::to_string(newHeight) + ")");
				}
				SDL_UnlockMutex(SDL::getMutex());

				if (video->hasPerspective_ && dimensionsChangedOrTextureMissing) {
					if (video->perspective_gva_) {
						g_value_array_free(video->perspective_gva_);
						video->perspective_gva_ = nullptr;
					}
					std::array<Point2D, 4> perspectiveBox = { Point2D(video->perspectiveCorners_[0], video->perspectiveCorners_[1]), // top-left
						Point2D(video->perspectiveCorners_[2], video->perspectiveCorners_[3]), // top-right
						Point2D(video->perspectiveCorners_[4], video->perspectiveCorners_[5]), // bottom-left
						Point2D(video->perspectiveCorners_[6], video->perspectiveCorners_[7])  // bottom-right 
					};
					std::array<double, 9> mat = computePerspectiveMatrixFromCorners(newWidth, newHeight, perspectiveBox);

					video->perspective_gva_ = g_value_array_new(9);
					GValue val = G_VALUE_INIT;
					g_value_init(&val, G_TYPE_DOUBLE);
					for (const double element : mat) {
						g_value_set_double(&val, element);
						g_value_array_append(video->perspective_gva_, &val);
					}
					g_value_unset(&val);

					if (video->perspective_) {
						g_object_set(video->perspective_, "matrix", video->perspective_gva_, nullptr);
						LOG_DEBUG("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) + "): Updated perspective matrix for " + video->currentFile_);
					}
				}
				LOG_INFO("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) + "): Caps processed successfully for " + video->currentFile_ +
					" (" + std::to_string(newWidth) + "x" + std::to_string(newHeight) + ")");
			}
			else {
				LOG_WARNING("GStreamerVideo::padProbeCallback", "Session " + std::to_string(callbackSessionId) +
					": Failed to get valid dimensions from CAPS for " + video->currentFile_);
				video->hasError_.store(true); // Could mark as error if dimensions are critical
			}
		}
		else {
			LOG_WARNING("GStreamerVideo::padProbeCallback", "Session " + std::to_string(callbackSessionId) +
				": Caps event, but gst_event_parse_caps failed for " + video->currentFile_);
			video->hasError_.store(true);
		}

		// This probe has done its job for this session (or determined it's stale).
		// GStreamer will remove the actual probe due to GST_PAD_PROBE_REMOVE.
		// We don't need to manage video->padProbeId_ here as it's reset at the start of each play()
		video->padProbeId_ = 0; // Mark as no active probe from our side
		video->hasValidFrame_.store(true, std::memory_order_release); // Mark that we have a valid frame
		return GST_PAD_PROBE_REMOVE;
	}
	return GST_PAD_PROBE_OK; // Keep other event types if any (though we only expect CAPS here)
}

void GStreamerVideo::createSdlTexture() {
	int newWidth = width_.load(std::memory_order_acquire);
	int newHeight = height_.load(std::memory_order_acquire);

	// Validate dimensions
	if (newWidth <= 0 || newHeight <= 0) {
		LOG_ERROR("GStreamerVideo", "Invalid dimensions (" +
			std::to_string(newWidth) + "x" + std::to_string(newHeight) + ").");
		textureValid_.store(false, std::memory_order_release);
		return;
	}

	bool needNewTexture = !videoTexture_ || (textureWidth_ != newWidth || textureHeight_ != newHeight);

	// If dimensions changed, we need new video texture
	if (needNewTexture) {

		if (videoTexture_) {
			SDL_DestroyTexture(videoTexture_);
			videoTexture_ = nullptr;
		}
		texture_ = nullptr;  // Reset pointer since we destroyed the texture

		textureValid_.store(false, std::memory_order_release);

		// Create YUV texture for video
		videoTexture_ = SDL_CreateTexture(
			SDL::getRenderer(monitor_), sdlFormat_,
			SDL_TEXTUREACCESS_STREAMING, newWidth, newHeight);

		if (!videoTexture_) {
			LOG_ERROR("GStreamerVideo", "SDL_CreateTexture failed: " + std::string(SDL_GetError()));
			textureValid_.store(false, std::memory_order_release);
			return;
		}

		SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;
		SDL_SetTextureBlendMode(videoTexture_, blendMode);

	}

	textureWidth_.store(newWidth, std::memory_order_release);
	textureHeight_.store(newHeight, std::memory_order_release);
	textureValid_.store(true, std::memory_order_release);
}

void GStreamerVideo::volumeUpdate() {
	if (!isPlaying_ || !playbin_)
		return;

	// Clamp volume_ to valid range
	volume_ = std::min(volume_, 1.0f);

	// Gradually adjust currentVolume_ towards volume_
	if (currentVolume_ > volume_ || currentVolume_ + 0.005 >= volume_)
		currentVolume_ = volume_;
	else
		currentVolume_ += 0.005;

	// Determine mute state
	bool shouldMute = (currentVolume_ < 0.1);

	// Update volume only if it has changed and is not muted
	if (!shouldMute && currentVolume_ != lastSetVolume_)
	{
		gst_stream_volume_set_volume(
			GST_STREAM_VOLUME(playbin_),
			GST_STREAM_VOLUME_FORMAT_LINEAR,
			currentVolume_);
		lastSetVolume_ = currentVolume_;
	}

	// Update mute state only if it has changed
	if (shouldMute != lastSetMuteState_)
	{
		gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), shouldMute);
		lastSetMuteState_ = shouldMute;
	}
}


int GStreamerVideo::getHeight() {
	return height_.load(std::memory_order_relaxed);
}

int GStreamerVideo::getWidth() {
	return width_.load(std::memory_order_relaxed);
}

void GStreamerVideo::draw() {
	std::lock_guard<std::mutex> lock(drawMutex_);

	if (!isPlaying_.load(std::memory_order_acquire) || !hasValidFrame_.load(std::memory_order_acquire)) {
		return;
	}

	GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(videoSink_), 0);
	if (!sample) {
		return;
	}

	GstBuffer* buf = gst_sample_get_buffer(sample);
	if (!buf) {
		gst_sample_unref(sample);
		return;
	}

	GstCaps const* caps = gst_sample_get_caps(sample);
	if (!caps) {
		gst_sample_unref(sample);
		return;
	}

	GstVideoInfo freshInfo;
	if (!gst_video_info_from_caps(&freshInfo, caps)) {
		gst_sample_unref(sample);
		return;
	}


	uint64_t currentGeneration = mappingGeneration_.fetch_add(1, std::memory_order_seq_cst) + 1;

	GstVideoFrame frame;
	bool mapped = gst_video_frame_map(&frame, &freshInfo, buf, GST_MAP_READ);

	if (!mapped) {
		gst_sample_unref(sample);
		return;
	}

	// Lazy texture creation
	if (!textureValid_.load(std::memory_order_acquire)) {
		createSdlTexture();
		if (!textureValid_.load(std::memory_order_acquire) || !videoTexture_) {
			gst_video_frame_unmap(&frame);
			gst_sample_unref(sample);
			return;
		}
	}

	// Critical generation check
	if (mappingGeneration_.load(std::memory_order_seq_cst) != currentGeneration) {
		LOG_WARNING("GStreamerVideo", "Stale mapping detected (Thread ID: %lu). Aborting texture update.");
		gst_video_frame_unmap(&frame);
		gst_sample_unref(sample);
		return;
	}

	// Perform texture update on videoTexture_
	bool success = updateTextureFunc_(videoTexture_, &frame);
	if (!success) {
		textureValid_.store(false, std::memory_order_release);
	}
	else if (texture_ != videoTexture_) {
		texture_ = videoTexture_;
		LOG_DEBUG("GStreamerVideo", "Promoted videoTexture_ to active texture_");
	}

	gst_video_frame_unmap(&frame);
	gst_sample_unref(sample);
}

bool GStreamerVideo::updateTextureFromFrameIYUV(SDL_Texture * texture, GstVideoFrame * frame) const {
	const Uint8* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const Uint8* srcU = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));
	const Uint8* srcV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 2));

	// Stride IS the distance between rows in the source buffer
	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);
	const int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 2);

	// Get the actual video dimensions from the frame
	const int frame_width = GST_VIDEO_FRAME_WIDTH(frame);
	const int frame_height = GST_VIDEO_FRAME_HEIGHT(frame);

	SDL_Rect frame_rect = { 0, 0, frame_width, frame_height };

	// Update the texture using the explicit frame rectangle
	if (SDL_UpdateYUVTexture(texture, &frame_rect, srcY, strideY, srcU, strideU, srcV, strideV) != 0) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_UpdateYUVTexture failed: ") + SDL_GetError());
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameNV12(SDL_Texture * texture, GstVideoFrame * frame) const {
	const uint8_t* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const uint8_t* srcUV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));

	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideUV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);

	// Get the actual video dimensions from the frame
	const int frame_width = GST_VIDEO_FRAME_WIDTH(frame);
	const int frame_height = GST_VIDEO_FRAME_HEIGHT(frame);

	SDL_Rect frame_rect = { 0, 0, frame_width, frame_height };

	// Use SDL_UpdateNVTexture for NV12 format
	if (SDL_UpdateNVTexture(texture, &frame_rect, srcY, strideY, srcUV, strideUV) != 0) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_UpdateNVTexture failed: ") + SDL_GetError());
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameRGBA(SDL_Texture * texture, GstVideoFrame * frame) const {
	void* pixels = nullptr;
	int pitch = 0;
	if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) != 0)
		return false;

	uint8_t* dst = static_cast<uint8_t*>(pixels);

	const int width = GST_VIDEO_FRAME_COMP_WIDTH(frame, 0);
	const int height = GST_VIDEO_FRAME_COMP_HEIGHT(frame, 0);

	const uint8_t* src = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);

	// --- Copy RGBA plane ---
	for (int y = 0; y < height; ++y) {
		SDL_memcpy(dst + y * pitch, src + y * stride, width * 4); // 4 bytes per pixel
	}

	SDL_UnlockTexture(texture);
	return true;
}

bool GStreamerVideo::isPlaying() {
	return isPlaying_.load(std::memory_order_acquire);
}

void GStreamerVideo::setVolume(float volume) {
	volume_ = volume;
}

void GStreamerVideo::skipForward() {
	if (!isPlaying_)
		return;
	gint64 current;
	gint64 duration;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration))
		return;
	current += 60 * GST_SECOND;
	if (current > duration)
		current = duration - 1;
	gst_element_seek_simple(playbin_, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		current);
}

void GStreamerVideo::skipBackward() {
	if (!isPlaying_)
		return;
	gint64 current;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (current > 60 * GST_SECOND)
		current -= 60 * GST_SECOND;
	else
		current = 0;
	gst_element_seek_simple(playbin_, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		current);
}

void GStreamerVideo::skipForwardp() {
	if (!isPlaying_)
		return;
	gint64 current;
	gint64 duration;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration))
		return;
	current += duration / 20;
	if (current > duration)
		current = duration - 1;
	gst_element_seek_simple(playbin_, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		current);
}

void GStreamerVideo::skipBackwardp() {

	if (!isPlaying_)
		return;
	gint64 current;
	gint64 duration;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration))
		return;
	if (current > duration / 20)
		current -= duration / 20;
	else
		current = 0;
	gst_element_seek_simple(playbin_, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		current);
}

void GStreamerVideo::pause() {
	if (!isPlaying_ || !playbin_) return;

	if (targetState_ == IVideo::VideoState::Paused)
		return;

	targetState_ = IVideo::VideoState::Paused;

	LOG_DEBUG("GStreamerVideo", "Requesting PAUSED for " + currentFile_);
	if (gst_element_set_state(playbin_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PAUSED for " + currentFile_);
		hasError_.store(true);
		targetState_ = IVideo::VideoState::None;
	}
}

void GStreamerVideo::resume() {
	if (!isPlaying_ || !playbin_) return;

	if (targetState_ == IVideo::VideoState::Playing)
		return;  // Already requested

	targetState_ = IVideo::VideoState::Playing;

	LOG_DEBUG("GStreamerVideo", "Requesting PLAYING for " + currentFile_);
	if (gst_element_set_state(playbin_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PLAYING for " + currentFile_);
		hasError_.store(true);
		targetState_ = IVideo::VideoState::None; // Reset fallback
	}
}

void GStreamerVideo::restart() {
	if (!isPlaying_)
		return;
	if (!gst_element_seek(playbin_, 1.0, GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH,
		GST_SEEK_TYPE_SET, 0,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
		LOG_ERROR("GStreamerVideo", "Failed to seek to start");
	}
}

unsigned long long GStreamerVideo::getCurrent() {
	gint64 ret = 0;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &ret) || !isPlaying_)
		ret = 0;
	return (unsigned long long)ret;
}

unsigned long long GStreamerVideo::getDuration() {
	gint64 ret = 0;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &ret) || !isPlaying_)
		ret = 0;
	return (unsigned long long)ret;
}

bool GStreamerVideo::isPaused() {
	return paused_.load(std::memory_order_acquire);
}

std::string GStreamerVideo::generateDotFileName(const std::string & prefix, const std::string & videoFilePath) const {
	std::string videoFileName = Utils::getFileName(videoFilePath);

	auto now = std::chrono::system_clock::now();
	auto now_c = std::chrono::system_clock::to_time_t(now);
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

	std::stringstream ss;
	ss << prefix << "_" << videoFileName << "_" << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S_")
		<< std::setfill('0') << std::setw(6) << microseconds.count();

	return ss.str();
}

void GStreamerVideo::enablePlugin(const std::string & pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory)
	{
		// Sets the plugin rank to PRIMARY + 1 to prioritize its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 1);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::disablePlugin(const std::string & pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory)
	{
		// Sets the plugin rank to GST_RANK_NONE to disable its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::setSoftOverlay(bool value) {
	softOverlay_ = value;
}

void GStreamerVideo::setPerspectiveCorners(const int* corners) {
	if (corners) {
		std::copy(corners, corners + 8, perspectiveCorners_);
		hasPerspective_ = true;  // Set flag when corners are initialized
	}
	else {
		std::fill(perspectiveCorners_, perspectiveCorners_ + 8, 0);
		hasPerspective_ = false;  // Reset flag when corners are cleared
	}
}

void GStreamerVideo::customGstLogHandler(GstDebugCategory * category, GstDebugLevel level,
	const gchar * file, const gchar * function, gint line,
	GObject * object, GstDebugMessage * message, gpointer user_data) {
	std::string logMsg = gst_debug_message_get(message);
	std::string componentName = (category && gst_debug_category_get_name(category)) ? gst_debug_category_get_name(category) : "Unknown";

	std::string component = "GStreamerLog";
	std::string finalMessage = componentName + ": " + logMsg;

	// Try to associate the log with a playing file
	if (object) {
		if (GstObject* gstObj = GST_OBJECT(object)) {
			if (GStreamerVideo* owner = findInstanceFromGstObject(gstObj)) {
				if (!owner->currentFile_.empty()) {
					std::string relativePath = owner->currentFile_;
					const std::string& basePath = Configuration::absolutePath;

					// Remove base path if it matches
					if (relativePath.find(basePath) == 0) {
						relativePath = relativePath.substr(basePath.length());
						if (!relativePath.empty() && (relativePath[0] == '/' || relativePath[0] == '\\')) {
							relativePath.erase(0, 1);  // Trim leading separator
						}
					}

					finalMessage = "[" + relativePath + "] " + finalMessage;
				}
			}
		}
	}

	// Map log level to your logging macros
	switch (level) {
		case GST_LEVEL_ERROR:   LOG_ERROR(component, finalMessage);   break;
		case GST_LEVEL_WARNING: LOG_WARNING(component, finalMessage); break;
		case GST_LEVEL_FIXME:   LOG_NOTICE(component, finalMessage);  break;
		case GST_LEVEL_INFO:    LOG_INFO(component, finalMessage);    break;
		case GST_LEVEL_DEBUG:
		case GST_LEVEL_LOG:
		case GST_LEVEL_TRACE:
		case GST_LEVEL_MEMDUMP:
		default:                LOG_DEBUG(component, finalMessage);  break;
	}
}

GStreamerVideo* GStreamerVideo::findInstanceFromGstObject(GstObject * object) {
	if (!object)
		return nullptr;

	GstObject* cur = object;
	while (cur) {
		std::lock_guard<std::mutex> lock(activeVideosMutex_);
		for (GStreamerVideo* video : activeVideos_) {
			if (video->playbin_ == GST_ELEMENT(cur)) {
				return video;
			}
		}
		cur = GST_OBJECT_PARENT(cur);
	}
	return nullptr;
}