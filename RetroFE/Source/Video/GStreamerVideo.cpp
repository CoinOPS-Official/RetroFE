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
}


GStreamerVideo::~GStreamerVideo() {
	try {
		stop();
	}
	catch (...) {
		// Destructor must not throw. Optionally log the error.
		LOG_ERROR("GStreamerVideo", "Exception in destructor during stop()");
	}
}

gboolean GStreamerVideo::busCallback(GstBus* bus, GstMessage* msg, gpointer user_data) {
	auto* video = static_cast<GStreamerVideo*>(user_data);

	if (!video) {
		LOG_ERROR("GStreamerVideo", "busCallback(): Callback invoked with null user_data.");
		return TRUE;
	}
	if (!video->playbin_) {
		LOG_WARNING("GStreamerVideo", "busCallback(): Callback invoked but playbin_ is null for file: " + video->currentFile_);
		return TRUE; // Continue, but state is unusual.
	}

	switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_STATE_CHANGED: {
			// Ensure the state change is from our playbin element
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->playbin_)) {
				GstState oldState, newState, pending;
				gst_message_parse_state_changed(msg, &oldState, &newState, &pending);

				if (newState == GST_STATE_PLAYING) {
					video->actualState_ = IVideo::VideoState::Playing;
				}
				else if (newState == GST_STATE_PAUSED) {
					video->actualState_ = IVideo::VideoState::Paused;
				}
				if (newState == GST_STATE_NULL || newState == GST_STATE_READY) {
					bool expected =
						(newState == GST_STATE_NULL && video->targetState_ == IVideo::VideoState::None) ||
						(newState == GST_STATE_READY && video->targetState_ == IVideo::VideoState::None);

					if (!expected && video->pipeLineReady_) {
						LOG_WARNING("GStreamerVideo",
							"busCallback(): Pipeline for " + video->currentFile_ +
							" transitioned to " + std::string(gst_element_state_get_name(newState)) +
							" unexpectedly from " + std::string(gst_element_state_get_name(oldState)) +
							". Playback may have stopped.");
					}
					else {
						LOG_DEBUG("GStreamerVideo",
							"busCallback(): Pipeline for " + video->currentFile_ +
							" transitioned to " + std::string(gst_element_state_get_name(newState)) +
							" as requested.");
					}
					video->actualState_ = IVideo::VideoState::None;
				}
				LOG_DEBUG("GStreamerVideo", "busCallback(): State changed: " +
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
						video->pipeLineReady_ = false;
					}
				}
				else if (video->playbin_) {
					LOG_DEBUG("GStreamerVideo", "BusCallback: EOS received very early for " + video->currentFile_ + ". Not looping.");
					// Potentially an issue if EOS comes too fast, could indicate a problem loading the file.
					// video->hasError_.store(true, std::memory_order_release);
					// video->isPlaying_ = false;
				}
			}
			break;
		}
		case GST_MESSAGE_ERROR: {
			GError* err = nullptr;
			gchar* dbg_info = nullptr;

			gst_message_parse_error(msg, &err, &dbg_info);

			LOG_ERROR("GStreamerVideo", "busCallback(): GStreamer ERROR received for " + video->currentFile_ +
				" (Session: " + std::to_string(video->currentPlaySessionId_.load()) + "): " +
				(err ? err->message : "Unknown error") +
				(dbg_info ? (std::string(" | Debug: ") + dbg_info) : ""));

			video->hasError_.store(true, std::memory_order_release);
			video->pipeLineReady_ = false;
			video->targetState_ = IVideo::VideoState::None; // Reflect that it's no longer in a valid playing/paused state

			if (err) g_error_free(err);
			if (dbg_info) g_free(dbg_info);
			break;
		}

		case GST_MESSAGE_APPLICATION: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->playbin_)) {
				const GstStructure* s = gst_message_get_structure(msg);

				// Check for the message posted by padProbeCallback
				if (s && gst_structure_has_name(s, "dimensions")) {
					int w = 0, h = 0;
					gst_structure_get_int(s, "width", &w);
					gst_structure_get_int(s, "height", &h);
					LOG_INFO("GStreamerVideo", "busCallback(): Received 'dimensions' message for " + video->currentFile_ +
						" (" + std::to_string(w) + "x" + std::to_string(h) + ")");
					video->width_ = w;
					video->height_ = h;
					video->createSdlTexture();
				}
			} // End of: if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->playbin_))
			break; // Break for case GST_MESSAGE_APPLICATION
		} // End of case GST_MESSAGE_APPLICATION

		default:
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
	SDL_LockMutex(SDL::getMutex()); // Protects read of texture_
	SDL_Texture* tex_to_render = texture_;
	SDL_UnlockMutex(SDL::getMutex());
	return tex_to_render;
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
	initialized_ = true;
	return true;
}

bool GStreamerVideo::deInitialize() {
	gst_deinit();
	initialized_ = false;
	return true;
}

bool GStreamerVideo::stop() {
	std::string currentFileForLog = currentFile_; // Cache for logging before it's cleared
	LOG_INFO("GStreamerVideo", "Stop called for " + (!currentFileForLog.empty() ? currentFileForLog : "unspecified video") +
		" (Session: " + std::to_string(currentPlaySessionId_.load()) + ")");

	// --- 1. Signal No Longer Playing ---
	// This should be done early to prevent other threads (e.g., draw, bus callback)
	// from attempting operations on a pipeline that's being torn down.
	pipeLineReady_ = false;
	targetState_ = IVideo::VideoState::None;

	// --- 2. GStreamer Pipeline Teardown ---
	// This lock protects GStreamer element operations and GStreamer-related shared state.
	std::lock_guard<std::mutex> lock(pipelineMutex_);

	if (playbin_) {
		// Remove active pad probe if one was registered for the current session
		if (padProbeId_ != 0 && videoSink_) {
			GstPad* sinkPad = gst_element_get_static_pad(GST_ELEMENT(videoSink_), "sink");
			if (sinkPad) {
				LOG_DEBUG("GStreamerVideo::stop", "Removing active pad probe ID " + std::to_string(padProbeId_));
				gst_pad_remove_probe(sinkPad, padProbeId_);
				gst_object_unref(sinkPad);
			}
			else {
				LOG_WARNING("GStreamerVideo::stop", "Could not get sink pad to remove probe during stop.");
			}
			// padProbeId_ will be reset with other members later.
		}

		// Remove bus watch
		if (busWatchId_ != 0) {
			g_source_remove(busWatchId_);
			// busWatchId_ will be reset with other members later.
			LOG_DEBUG("GStreamerVideo::stop", "Removed bus watch.");
		}

		// Disconnect signal handlers (like element-setup)
		if (elementSetupHandlerId_ != 0) {
			// Check if connected before disconnecting, to avoid warnings if already disconnected
			if (g_signal_handler_is_connected(playbin_, elementSetupHandlerId_)) {
				g_signal_handler_disconnect(playbin_, elementSetupHandlerId_);
			}
			// elementSetupHandlerId_ will be reset with other members later.
			LOG_DEBUG("GStreamerVideo::stop", "Disconnected element-setup signal handler.");
		}

		stagedSample_.clear();

		// Set the pipeline state to NULL to release all GStreamer resources.
		LOG_DEBUG("GStreamerVideo::stop", "Setting playbin state to NULL.");
		GstStateChangeReturn setStateRet = gst_element_set_state(playbin_, GST_STATE_NULL);

		if (setStateRet == GST_STATE_CHANGE_FAILURE) {
			LOG_ERROR("GStreamerVideo::stop", "Failed to set playbin state to NULL. Resources may not be fully released by GStreamer.");
		}
		else {
			// Optionally wait for NULL state to ensure proper cleanup.
			GstState currentStateRead = GST_STATE_VOID_PENDING; // Default to something other than NULL
			GstStateChangeReturn getStateRet = gst_element_get_state(playbin_, &currentStateRead, nullptr, 2 * GST_SECOND); // 2-second timeout
			if (getStateRet == GST_STATE_CHANGE_SUCCESS && currentStateRead == GST_STATE_NULL) {
				LOG_DEBUG("GStreamerVideo::stop", "Playbin successfully transitioned to NULL.");
			}
			else {
				LOG_WARNING("GStreamerVideo::stop", "Playbin did not confirm NULL state (or timed out). GetStateReturn: " +
					std::string(gst_element_state_change_return_get_name(getStateRet)) +
					", Current Actual State: " + std::string(gst_element_state_get_name(currentStateRead)));
			}
		}

		// Unreference the playbin. This will destroy all elements it contains.
		LOG_DEBUG("GStreamerVideo::stop", "Unreffing playbin_.");
		gst_object_unref(playbin_);
		playbin_ = nullptr;
		videoSink_ = nullptr;   // Was part of playbin_
		perspective_ = nullptr; // Was part of playbin_

	}
	else {
		LOG_DEBUG("GStreamerVideo", "stop(): No playbin_ was active to stop and unref.");
	}

	// --- 3. SDL Texture Cleanup
	// This lock protects all SDL texture objects and their associated state.
	SDL_LockMutex(SDL::getMutex());

	if (videoTexture_) {
		SDL_DestroyTexture(videoTexture_);
		videoTexture_ = nullptr;
		LOG_DEBUG("GStreamerVideo", "stop (): Destroyed videoTexture_.");
	}

	texture_ = nullptr; // Since both potential source textures are gone, texture_ must be null.
	// The renderer must be able to handle a null texture from getTexture().
	SDL_UnlockMutex(SDL::getMutex());

	hasError_.store(false, std::memory_order_release);


	width_ = -1;
	height_ = -1;

	currentFile_.clear();
	playCount_ = 0;
	numLoops_ = 0; // Assuming loops are reset on full stop

	// Reset volume state
	currentVolume_ = 0.0f;
	lastSetVolume_ = -1.0f; // Force re-application if volume is set again
	lastSetMuteState_ = true; // Sensible default to start muted
	volume_ = 0.0f;         // Desired volume reset

	// Reset GStreamer IDs
	padProbeId_ = 0;
	busWatchId_ = 0;
	elementSetupHandlerId_ = 0;
	// currentPlaySessionId_ will be updated by the static generator on the next play() call.

	// Free GStreamer GValueArray for perspective matrix
	if (perspective_gva_) {
		g_value_array_free(perspective_gva_);
		perspective_gva_ = nullptr;
	}

	// Any other custom member variables specific to a playback session should be reset.

	LOG_INFO("GStreamerVideo", "stop (): Instance for " + (!currentFileForLog.empty() ? currentFileForLog : "previous video") +
		" fully stopped, all GStreamer and SDL resources released.");
	return true;
}

bool GStreamerVideo::unload() {
	LOG_DEBUG("GStreamerVideo", "Unload called for " + currentFile_ + " (Session: " + std::to_string(currentPlaySessionId_.load()) + ")");

	// --- 1. Initial State Check ---
	if (!playbin_) { // If no pipeline, nothing to unload from GStreamer's perspective
		LOG_WARNING("GStreamerVideo", "unload(): No playbin_ to unload for " + currentFile_);
		// Ensure local state is reset even if playbin_ was already gone
		pipeLineReady_ = false;
		hasError_.store(false, std::memory_order_release); // Clear any previous error
		width_ = 0;
		height_ = 0;
		SDL_LockMutex(SDL::getMutex());
		texture_ = nullptr; // Default to nullptr
		SDL_UnlockMutex(SDL::getMutex());
		currentFile_.clear();
		playCount_ = 0;
		// nextUniquePlaySessionId_ is static, not reset per instance
		// currentPlaySessionId_ will be updated on next play()
		return true; // Or false if this state is unexpected
	}

	std::lock_guard<std::mutex> lock(pipelineMutex_);

	// Reset instance state for new playback
	width_ = 0;
	height_ = 0;

	SDL_LockMutex(SDL::getMutex());
	texture_ = nullptr;
	// DO NOT destroy videoTexture_ here, so reuse is possible.
	SDL_UnlockMutex(SDL::getMutex());

	currentFile_.clear();
	playCount_ = 0;
	numLoops_ = 0;

	currentVolume_ = 0.0f;
	lastSetVolume_ = -1.0f;
	lastSetMuteState_ = false; // or true if you want muted as default
	volume_ = 0.0f;

	playCount_ = 0;

	// --- 2. Signal that we are no longer "playing" this specific stream ---
	pipeLineReady_ = false;
	targetState_ = IVideo::VideoState::None; // Reflect that it's not actively playing or paused

	// --- 3. GStreamer Pipeline Management ---
	// Remove active pad probe if one was registered by this instance for the current session
	if (padProbeId_ != 0 && videoSink_) {
		GstPad* sinkPad = gst_element_get_static_pad(GST_ELEMENT(videoSink_), "sink");
		if (sinkPad) {
			LOG_DEBUG("GStreamerVideo", "unload(): Active pad probe ID " + std::to_string(padProbeId_) + " for " + currentFile_);
			gst_pad_remove_probe(sinkPad, padProbeId_);
			gst_object_unref(sinkPad);
		}
		else {
			LOG_WARNING("GStreamerVideo", "unload(): Could not get sink pad to remove probe for " + currentFile_);
		}
		padProbeId_ = 0; // Mark as no active probe from our side
	}

	// Remove bus watch
	if (busWatchId_ != 0) {
		g_source_remove(busWatchId_);
		busWatchId_ = 0;
		LOG_DEBUG("GStreamerVideo", "unload(): Removed bus watch for " + currentFile_);
	}

	stagedSample_.clear();

	// Set pipeline to GST_STATE_READY to release resources but keep pipeline structure for reuse
	LOG_DEBUG("GStreamerVideo", "unload(): Setting playbin state to READY for " + currentFile_);
	GstStateChangeReturn setStateRet = gst_element_set_state(playbin_, GST_STATE_READY);

	GstState currentStateRead = GST_STATE_VOID_PENDING;
	GstStateChangeReturn getStateRet = gst_element_get_state(
		playbin_, &currentStateRead, nullptr, 2 * GST_SECOND); // Wait up to 2 seconds

	if (getStateRet == GST_STATE_CHANGE_SUCCESS && currentStateRead == GST_STATE_READY) {
		LOG_DEBUG("GStreamerVideo", "playbin_ confirmed in READY state for " + currentFile_);
	}
	else {
		LOG_WARNING("GStreamerVideo", "playbin_ did not confirm READY state (or timed out). GetStateReturn: " +
			std::string(gst_element_state_change_return_get_name(getStateRet)) +
			", Actual State: " + std::string(gst_element_state_get_name(currentStateRead)));
	}

	// Flush the bus to discard any pending messages from the previous playback
	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
	if (bus) {
		gst_bus_set_flushing(bus, TRUE); // Start flushing
		gst_object_unref(bus);
		LOG_DEBUG("GStreamerVideo", "unload(): Flushed bus for " + currentFile_);
	}

	// --- 4. Reset Instance State for Reuse ---
	hasError_.store(false, std::memory_order_release); // Clear error from previous playback

	// Reset GStreamer specific data that might be tied to the previous stream's content
	// perspective_gva_ was calculated based on previous video's dimensions.
	if (perspective_gva_) {
		g_value_array_free(perspective_gva_);
		perspective_gva_ = nullptr;
		LOG_DEBUG("GStreamerVideo", "unload(): Freed perspective_gva_ for " + currentFile_);
	}

	return true;
}

// Main function to compute perspective transform from 4 arbitrary points
static inline std::array<double, 9> computePerspectiveMatrixFromCorners(
	int width,
	int height,
	const std::array<Point2D, 4>& pts) {
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
	g_object_set(playbin_, "flags", flags, "instant-uri", TRUE, nullptr);

	// Configure appsink.
	g_object_set(videoSink_,
		"max-buffers", 1,  // Only keep latest buffer.
		"drop", TRUE,      // Drop old buffers.
		"wait-on-eos", FALSE,
		"qos", FALSE,
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


	GstAppSinkCallbacks callbacks = {};
	callbacks.eos = nullptr; // You can add an EOS handler if you want
	callbacks.new_preroll = nullptr; // You can handle preroll if needed
	callbacks.new_sample = &GStreamerVideo::on_new_sample; // Or just on_new_sample if static

	gst_app_sink_set_callbacks(
		GST_APP_SINK(videoSink_), // your appsink pointer
		&callbacks,
		this,     // user_data, e.g., your GStreamerVideo instance
		nullptr   // GDestroyNotify
	);
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
		updateTextureFunc_ = [](SDL_Texture const*, GstVideoFrame const*) {
			return false;
			};
		break;
	}
}

bool GStreamerVideo::play(const std::string& file) {
	std::lock_guard<std::mutex> lock(pipelineMutex_);

	if (!initialized_) {
		LOG_ERROR("GStreamerVideo", "Play called but GStreamer not initialized for file: " + file);
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	// Atomically increment and assign a new unique session ID for this play attempt.
	// This ID is unique across all GStreamerVideo instances and all play attempts.
	currentPlaySessionId_.store(nextUniquePlaySessionId_++, std::memory_order_release);

	currentFile_ = file;

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
		auto* userdataForProbe = static_cast<PadProbeUserdata*>(g_malloc(sizeof(PadProbeUserdata)));
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
			g_free);

		gst_object_unref(sinkPad);
	}
	else {
		LOG_ERROR("GStreamerVideo", "Failed to get sink pad from videoSink_ for file: " + file);
		hasError_.store(true);
		return false;
	}

	GstBus* bus = gst_element_get_bus(playbin_);
	if (bus) {
		gst_bus_set_flushing(bus, FALSE); // Ensure bus is not in flushing state before adding watch
		busWatchId_ = gst_bus_add_watch(bus, GStreamerVideo::busCallback, this);
		gst_object_unref(bus);
	}
	// Convert file path to URI
	gchar* uriFile = gst_filename_to_uri(file.c_str(), nullptr);
	if (!uriFile) {
		LOG_DEBUG("Video", "Failed to convert filename to URI");
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	g_object_set(videoSink_, "sync", FALSE, nullptr);

	// Update URI - no need to set to READY first
	g_object_set(playbin_, "uri", uriFile, nullptr);
	g_free(uriFile);

	targetState_ = IVideo::VideoState::Paused;  // accurately reflect that we're starting preloaded

	GstStateChangeReturn stateRet = gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PAUSED);

	if (stateRet == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Pipeline failed to set PAUSED for " + file);
		pipeLineReady_ = false;
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	// If it's ASYNC, that's expected — the bus callback will confirm when PAUSED is reached.
	if (stateRet == GST_STATE_CHANGE_ASYNC) {
		LOG_DEBUG("GStreamerVideo", "Pipeline state change to PAUSED is async for " + file);
	}
	else {
		LOG_DEBUG("GStreamerVideo", "Pipeline set to PAUSED immediately for " + file);
	}

	pipeLineReady_ = true;

	// Mute and volume to 0 by default
	gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_), GST_STREAM_VOLUME_FORMAT_LINEAR, 0.0);
	gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), true);
	lastSetMuteState_ = true;

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

void GStreamerVideo::elementSetupCallback([[maybe_unused]] GstElement* playbin, GstElement* element, [[maybe_unused]] gpointer data) {
	// Check if the element is a video decoder
	if (!Configuration::HardwareVideoAccel && GST_IS_VIDEO_DECODER(element))
	{
		// Configure the video decoder
		g_object_set(element, "thread-type", Configuration::AvdecThreadType,
			"max-threads", Configuration::AvdecMaxThreads,
			"direct-rendering", FALSE, "std-compliance", 0, nullptr);
	}
}

GstFlowReturn GStreamerVideo::on_new_sample(GstAppSink* sink, gpointer userData) {
	auto* self = static_cast<GStreamerVideo*>(userData);

	GstSample* sample = gst_app_sink_pull_sample(sink);
	if (!sample) {
		return GST_FLOW_FLUSHING;  // EOS or error
	}

	if (!self->stagedSample_.try_enqueue(sample)) {
		gst_sample_unref(sample);  // Drop frame if not consumed
	}

	return GST_FLOW_OK;
}

GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
	auto* probeData = static_cast<PadProbeUserdata*>(user_data);

	// --- 1. Basic Probe Sanity Checks ---
	if (!probeData || !probeData->videoInstance) {
		LOG_ERROR("GStreamerVideo", "padProbeCallback(): Probe fired with invalid user_data or videoInstance.");
		return GST_PAD_PROBE_REMOVE;
	}

	GStreamerVideo* video = probeData->videoInstance;
	uint64_t callbackSessionId = probeData->playSessionId;

	// --- 2. Session ID Check (Crucial for Stale Probes) ---
	uint64_t videoCurrentSessionId = video->currentPlaySessionId_.load(std::memory_order_acquire);
	if (videoCurrentSessionId != callbackSessionId) {
		LOG_ERROR("GStreamerVideo", "padProbeCallback(): Stale probe for session ID: " + std::to_string(callbackSessionId) +
			" (current: " + std::to_string(videoCurrentSessionId) + " for file: " + video->currentFile_ + "). Removing probe.");
		return GST_PAD_PROBE_REMOVE;
	}

	// --- 3. Instance State Check (Is this GStreamerVideo instance still actively playing?) ---
	if (!video->playbin_) {
		LOG_ERROR("GStreamerVideo", "padProbeCallback(): Probe for session ID: " + std::to_string(callbackSessionId) +
			" fired, but playbin_ is NULL or video not playing (file: " + video->currentFile_ + "). Removing probe.");
		video->padProbeId_ = 0; // Mark that this instance no longer expects this probe to be active.
		return GST_PAD_PROBE_REMOVE;
	}

	// --- 4. Handle CAPS Event ---
	auto* event = GST_PAD_PROBE_INFO_EVENT(info);
	if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
		GstCaps* caps = nullptr;
		gst_event_parse_caps(event, &caps);

		if (caps) {
			const GstStructure* s = gst_caps_get_structure(caps, 0);
			int newWidth = 0;
			int	newHeight = 0;

			if (gst_structure_get_int(s, "width", &newWidth) &&
				gst_structure_get_int(s, "height", &newHeight) &&
				newWidth > 0 && newHeight > 0) {

				LOG_INFO("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) +
					"): Caps received for " + video->currentFile_ + " (" +
					std::to_string(newWidth) + "x" + std::to_string(newHeight) + ")");

				// Instead of writing to atomics, just post a message with these values
				GstStructure* msg_struct = gst_structure_new(
					"dimensions",
					"width", G_TYPE_INT, newWidth,
					"height", G_TYPE_INT, newHeight,
					nullptr
				);
				GstBus* bus = gst_element_get_bus(video->playbin_);
				if (bus) {
					gst_bus_post(bus, gst_message_new_application(GST_OBJECT(video->playbin_), msg_struct));
					gst_object_unref(bus);
				}
				else {
					gst_structure_free(msg_struct);
					LOG_ERROR("GStreamerVideo", "padProbeCallback(): Failed to get bus to post 'dimensions' message.");
				}

				// --- Perspective Matrix Calculation and Setting (if applicable) ---
				if (video->hasPerspective_) {
					if (!video->perspective_) {
						LOG_ERROR("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) +
							"): hasPerspective_ is true, but perspective_ GstElement is NULL for " + video->currentFile_);
					}
					else {
						if (video->perspective_gva_) {
							g_value_array_free(video->perspective_gva_);
							video->perspective_gva_ = nullptr;
						}
						std::array<Point2D, 4> perspectiveBox = {
							Point2D(static_cast<double>(video->perspectiveCorners_[0]), static_cast<double>(video->perspectiveCorners_[1])),
							Point2D(static_cast<double>(video->perspectiveCorners_[2]), static_cast<double>(video->perspectiveCorners_[3])),
							Point2D(static_cast<double>(video->perspectiveCorners_[4]), static_cast<double>(video->perspectiveCorners_[5])),
							Point2D(static_cast<double>(video->perspectiveCorners_[6]), static_cast<double>(video->perspectiveCorners_[7]))
						};
						std::array<double, 9> mat = computePerspectiveMatrixFromCorners(newWidth, newHeight, perspectiveBox);

						LOG_DEBUG("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) +
							") Calculated Matrix for " + video->currentFile_ + ": [" + /* ... log matrix ... */ "]");

						video->perspective_gva_ = g_value_array_new(9);
						GValue val = G_VALUE_INIT;
						g_value_init(&val, G_TYPE_DOUBLE);
						for (const double element : mat) {
							g_value_set_double(&val, element);
							g_value_array_append(video->perspective_gva_, &val);
						}
						g_value_unset(&val);

						g_object_set(G_OBJECT(video->perspective_), "matrix", video->perspective_gva_, nullptr);
						LOG_DEBUG("GStreamerVideo", "PadProbe (Session " + std::to_string(callbackSessionId) +
							"): Updated perspective matrix on GstElement " + GST_OBJECT_NAME(video->perspective_) + " for " + video->currentFile_);
					}
				}

			}
			else { // Failed to get width/height from CAPS
				LOG_ERROR("GStreamerVideo", "padProbeCallback(): Session " + std::to_string(callbackSessionId) +
					": Failed to get valid dimensions from CAPS structure for " + video->currentFile_);
				video->hasError_.store(true, std::memory_order_release);
			}
		}
		else { // gst_event_parse_caps failed
			LOG_ERROR("GStreamerVideo", "padProbeCallback(): Session " + std::to_string(callbackSessionId) +
				": Caps event, but gst_event_parse_caps failed (returned NULL caps) for " + video->currentFile_);
			video->hasError_.store(true, std::memory_order_release);
		}
		video->padProbeId_ = 0; // This probe has done its job for this CAPS event.
		return GST_PAD_PROBE_REMOVE; // Remove the probe.
	}

	// --- 5. For other event types (not expected with GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM for CAPS) ---
	return GST_PAD_PROBE_OK;
}

void GStreamerVideo::createSdlTexture() {

	if (width_ <= 0 || height_ <= 0) {
		LOG_ERROR("GStreamerVideo", "createSdlTexture(): Cannot create texture, target dimensions are invalid: "
			+ std::to_string(width_) + "x" + std::to_string(height_));
		if (videoTexture_) {
			SDL_DestroyTexture(videoTexture_);
			videoTexture_ = nullptr;
		}
		textureWidth_ = -1;
		textureHeight_ = -1;
		return;
	}

	if (videoTexture_ &&
		textureWidth_ == width_ &&
		textureHeight_ == height_
		) {
		LOG_DEBUG("GStreamerVideo", "createSdlTexture(): Video texture already exists with correct dimensions "
			+ std::to_string(width_) + "x" + std::to_string(height_) + ". No recreation needed.");
		return;
	}

	LOG_INFO("GStreamerVideo", "createSdlTexture(): Creating/recreating SDL video texture for "
		+ std::to_string(width_) + "x" + std::to_string(height_) +
		" with format ID: " + std::to_string(sdlFormat_)); // Log the format

	if (videoTexture_) {
		SDL_DestroyTexture(videoTexture_);
		videoTexture_ = nullptr;
	}

	videoTexture_ = SDL_CreateTexture(
		SDL::getRenderer(monitor_),
		sdlFormat_, // The format determined by GStreamerVideo (e.g., IYUV, NV12, ABGR8888)
		SDL_TEXTUREACCESS_STREAMING,
		width_, height_);

	if (!videoTexture_) {
		LOG_ERROR("GStreamerVideo", "createSdlTexture(): SDL_CreateTexture failed for format " +
			std::string(SDL_GetPixelFormatName(sdlFormat_)) + ": " + std::string(SDL_GetError()));
		textureWidth_ = -1;
		textureHeight_ = -1;
		return;
	}

	SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;
	SDL_SetTextureBlendMode(videoTexture_, blendMode);

	textureWidth_ = width_;
	textureHeight_ = height_;
}

void GStreamerVideo::volumeUpdate() {
	if (!pipeLineReady_ || !playbin_)
		return;

	// Clamp volume_ to valid range
	volume_ = std::min(volume_, 1.0f);

	// Gradually adjust currentVolume_ towards volume_
	if (currentVolume_ > volume_ || currentVolume_ + 0.005 >= volume_)
		currentVolume_ = volume_;
	else
		currentVolume_ += 0.005;

	// Determine mute state
	bool shouldMute = (currentVolume_ < 0.1) || Configuration::MuteVideo;

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
	return height_;
}

int GStreamerVideo::getWidth() {
	return width_;
}

void GStreamerVideo::draw() {
	std::lock_guard<std::mutex> pipelineLock(pipelineMutex_);
	if (!pipeLineReady_) return;

	GstSample* sample = stagedSample_.try_dequeue();
	if (!sample) return;

	GstVideoFrame frame{};
	
	bool frameMapped = false;

	SDL_LockMutex(SDL::getMutex());

	if (videoTexture_) {
		GstBuffer* buf = gst_sample_get_buffer(sample);
		const GstCaps* caps = gst_sample_get_caps(sample);
		GstVideoInfo info;
		gst_video_info_init(&info);

		if (buf && caps && gst_video_info_from_caps(&info, caps)) {
			frameMapped = gst_video_frame_map(&frame, &info, buf, GST_MAP_READ);
			if (frameMapped) {
				bool success = updateTextureFunc_(videoTexture_, &frame);
				if (success) {
					if (!texture_) texture_ = videoTexture_;
				}
				else {
					LOG_ERROR("GStreamerVideo::draw", "Texture update failed for " + currentFile_);
					texture_ = nullptr;
				}
			}
		}
	}

	SDL_UnlockMutex(SDL::getMutex());

	if (frameMapped) {
		gst_video_frame_unmap(&frame);
	}
	gst_sample_unref(sample);
}

bool GStreamerVideo::updateTextureFromFrameIYUV(SDL_Texture* texture, GstVideoFrame* frame) const {
	const auto* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const auto* srcU = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));
	const auto* srcV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 2));

	// Stride IS the distance between rows in the source buffer
	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);
	const int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 2);

	LOG_DEBUG("GStreamerVideo", "Before update, SDL_GetError()=" + std::string(SDL_GetError()));

	// Update the texture using the explicit frame rectangle
	if (SDL_UpdateYUVTexture(texture, nullptr, srcY, strideY, srcU, strideU, srcV, strideV) != 0) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_UpdateYUVTexture failed: ") + SDL_GetError());
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameNV12(SDL_Texture* texture, GstVideoFrame* frame) const {
	const auto* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const auto* srcUV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));

	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideUV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);

	// Use SDL_UpdateNVTexture for NV12 format
	if (SDL_UpdateNVTexture(texture, nullptr, srcY, strideY, srcUV, strideUV) != 0) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_UpdateNVTexture failed: ") + SDL_GetError());
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameRGBA(SDL_Texture* texture, GstVideoFrame* frame) const {

	const void* src_pixels = GST_VIDEO_FRAME_PLANE_DATA(frame, 0);
	const int src_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);

	if (SDL_UpdateTexture(texture, nullptr, src_pixels, src_pitch) != 0) {
		LOG_ERROR("GStreamerVideo", "SDL_UpdateTexture failed: " + std::string(SDL_GetError()));
		return false;
	}

	return true;
}

bool GStreamerVideo::isPlaying() {
	return actualState_ == IVideo::VideoState::Playing;
}

void GStreamerVideo::setVolume(float volume) {
	volume_ = volume;
}

void GStreamerVideo::skipForward() {
	if (!pipeLineReady_)
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
	if (!pipeLineReady_)
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
	if (!pipeLineReady_)
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

	if (!pipeLineReady_)
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
	if (!playbin_) return;

	// Only return early if already paused
	if (actualState_ == IVideo::VideoState::Paused)
		return;
	if (targetState_ == IVideo::VideoState::Paused)
		return;

	targetState_ = IVideo::VideoState::Paused;

	// Allow first frame to render even while paused
	if (videoSink_)
		g_object_set(videoSink_, "sync", FALSE, nullptr);

	LOG_DEBUG("GStreamerVideo", "Requesting PAUSED for " + currentFile_);
	if (gst_element_set_state(playbin_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PAUSED for " + currentFile_);
		hasError_.store(true);
	}
}

void GStreamerVideo::resume() {
	if (!playbin_) return;
	// Only return early if actual state is already playing
	if (actualState_ == IVideo::VideoState::Playing)
		return;
	// Already requested play?
	if (targetState_ == IVideo::VideoState::Playing)
		return;

	targetState_ = IVideo::VideoState::Playing;
	
	if (videoSink_)
		g_object_set(videoSink_, "sync", TRUE, nullptr);

	LOG_DEBUG("GStreamerVideo", "Requesting PLAYING for " + currentFile_);
	if (gst_element_set_state(playbin_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PLAYING for " + currentFile_);
		hasError_.store(true);
	}
}

void GStreamerVideo::restart() {
	if (!pipeLineReady_)
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
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &ret) || !pipeLineReady_)
		ret = 0;
	return (unsigned long long)ret;
}

unsigned long long GStreamerVideo::getDuration() {
	gint64 ret = 0;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &ret) || !pipeLineReady_)
		ret = 0;
	return (unsigned long long)ret;
}

bool GStreamerVideo::isPaused() {
	return getActualState() == IVideo::VideoState::Paused;
}

std::string GStreamerVideo::generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const {
	std::string videoFileName = Utils::getFileName(videoFilePath);

	auto now = std::chrono::system_clock::now();
	auto now_c = std::chrono::system_clock::to_time_t(now);
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

	std::stringstream ss;
	ss << prefix << "_" << videoFileName << "_" << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S_")
		<< std::setfill('0') << std::setw(6) << microseconds.count();

	return ss.str();
}

void GStreamerVideo::enablePlugin(const std::string& pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory)
	{
		// Sets the plugin rank to PRIMARY + 1 to prioritize its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 1);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::disablePlugin(const std::string& pluginName) {
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