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


bool GStreamerVideo::initialized_ = false;
bool GStreamerVideo::pluginsInitialized_ = false;

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
	SDL_LockMutex(SDL::getMutex());
	if (alphaTexture_) return;  // Already created

	alphaTexture_ = SDL_CreateTexture(
		SDL::getRenderer(monitor_), SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STREAMING, ALPHA_TEXTURE_SIZE, ALPHA_TEXTURE_SIZE);

	if (!alphaTexture_) {
		LOG_ERROR("GStreamerVideo", "Failed to create alpha texture: " + std::string(SDL_GetError()));
		return;
	}

	SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;
	SDL_SetTextureBlendMode(alphaTexture_, blendMode);

	// Initialize alpha texture to transparent black
	void* pixels;
	int pitch;
	if (SDL_LockTexture(alphaTexture_, nullptr, &pixels, &pitch) == 0) {
		SDL_memset(pixels, 0, pitch * ALPHA_TEXTURE_SIZE);
		SDL_UnlockTexture(alphaTexture_);
	}
	SDL_UnlockMutex(SDL::getMutex());
}

void GStreamerVideo::messageHandler() {
	if (!playbin_ || !isPlaying_)
		return;

	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
	if (!bus)
		return;

	// Process all pending messages (non-blocking)
	GstMessage* msg;
	while ((msg = gst_bus_pop(bus))) {
		switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_ERROR: {
			GError* err;
			gchar* debug_info;
			gst_message_parse_error(msg, &err, &debug_info);

			// Set error flag and log the error
			hasError_.store(true, std::memory_order_release);
			LOG_ERROR("GStreamerVideo", "Error received from element " +
				std::string(GST_OBJECT_NAME(msg->src)) + ": " +
				std::string(err->message));
			if (debug_info) {
				LOG_DEBUG("GStreamerVideo", "Debug info: " + std::string(debug_info));
			}

			g_clear_error(&err);
			g_free(debug_info);
			break;
		}
		case GST_MESSAGE_WARNING: {
			GError* err;
			gchar* debug_info;
			gst_message_parse_warning(msg, &err, &debug_info);
			LOG_DEBUG("GStreamerVideo", "Warning: " + std::string(err->message));
			g_clear_error(&err);
			g_free(debug_info);
			break;
		}
		case GST_MESSAGE_INFO: {
			GError* err;
			gchar* debug_info;
			gst_message_parse_info(msg, &err, &debug_info);
			LOG_DEBUG("GStreamerVideo", "Info: " + std::string(err->message));
			g_clear_error(&err);
			g_free(debug_info);
			break;
		}
		case GST_MESSAGE_EOS: {
			// Check for EOS only if more than 1 second has played
			if (getCurrent() > GST_SECOND) {
				playCount_++;
				if (!numLoops_ || numLoops_ > playCount_) {
					restart();
				}
				else {
					stop();
				}
			}
			break;
		}
		default:
			break;
		}
		gst_message_unref(msg);
	}
	gst_object_unref(bus);
}

void GStreamerVideo::initializePlugins() {
	if (!pluginsInitialized_)
	{
		pluginsInitialized_ = true;

#if defined(WIN32)
		enablePlugin("directsoundsink");
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

				LOG_DEBUG("GStreamerVideo", "Using qsvh264dec/qsvh265dec for Intel GPU");
			}
			else
			{
				enablePlugin("d3d11h264dec");
				enablePlugin("d3d11h265dec");
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
	if (initialized_)
	{
		initialized_ = true;
		return true;
	}
	if (!gst_is_initialized())
	{
		LOG_DEBUG("GStreamer", "Initializing in instance");
		gst_init(nullptr, nullptr);
		std::string path = Utils::combinePath(Configuration::absolutePath, "retrofe");
#ifdef WIN32
		GstRegistry* registry = gst_registry_get();
		gst_registry_scan_path(registry, path.c_str());
#endif
	}
	initialized_ = true;
	return true;
}

bool GStreamerVideo::deInitialize() {
	gst_deinit();
	initialized_ = false;
	paused_ = false;
	return true;
}

bool GStreamerVideo::stop() {
	if (!initialized_)
	{
		return false;
	}

	stopping_.store(true, std::memory_order_release);

	isPlaying_ = false;

	if (playbin_)
	{
		// Set the pipeline state to NULL
		gst_element_set_state(playbin_, GST_STATE_NULL);

		// Wait for the state change to complete
		GstState state;
		GstStateChangeReturn ret = gst_element_get_state(playbin_, &state, nullptr, GST_CLOCK_TIME_NONE);
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
	if (videoInfo_) {
		gst_video_info_free(videoInfo_);
		videoInfo_ = nullptr;
	}

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


	return true;
}


bool GStreamerVideo::unload() {
	if (!playbin_) {
		return false;
	}

	stopping_.store(true, std::memory_order_release);
	isPlaying_ = false;


	texture_ = alphaTexture_;  // Switch to transparent texture


	// Set pipeline to GST_STATE_READY (instead of GST_STATE_NULL) so we can reuse it later
	GstStateChangeReturn ret = gst_element_set_state(playbin_, GST_STATE_READY);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set pipeline to READY during unload.");
		return false;
	}

	// Optionally wait for the state change to complete 
	GstState newState;
	ret = gst_element_get_state(playbin_, &newState, nullptr, GST_SECOND);
	if (ret == GST_STATE_CHANGE_FAILURE || newState != GST_STATE_READY) {
		LOG_ERROR("GStreamerVideo", "Pipeline did not reach READY state during unload.");
	}

	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(playbin_));

	// Process all pending messages (non-blocking)
	GstMessage* msg;
	while ((msg = gst_bus_pop(bus))) {
		switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_ERROR: {
			GError* err;
			gchar* debug_info;
			gst_message_parse_error(msg, &err, &debug_info);

			// Set error flag and log the error
			hasError_.store(true, std::memory_order_release);
			LOG_ERROR("GStreamerVideo", "Error received from element " +
				std::string(GST_OBJECT_NAME(msg->src)) + ": " +
				std::string(err->message));
			if (debug_info) {
				LOG_DEBUG("GStreamerVideo", "Debug info: " + std::string(debug_info));
			}

			g_clear_error(&err);
			g_free(debug_info);
			break;
		}
		default:
			break;
		}
		gst_message_unref(msg);
	}
	gst_object_unref(bus);

	// Reset flags used for timing, volume, etc.
	paused_ = false;
	currentVolume_ = 0.0f;
	lastSetVolume_ = -1.0f;
	lastSetMuteState_ = false;
	volume_ = 0.0f;            // reset to default
	playCount_ = 0;
	numLoops_ = 0;

	SDL_LockMutex(SDL::getMutex());
	if (videoInfo_) {
		gst_video_info_free(videoInfo_);
		videoInfo_ = nullptr;
	}
	textureWidth_ = width_.load(std::memory_order_acquire);
	textureHeight_ = height_.load(std::memory_order_acquire);
	width_.store(0, std::memory_order_release);
	height_.store(0, std::memory_order_release);
	texture_ = alphaTexture_;  // Switch to blank texture
	textureValid_ = false;
	SDL_UnlockMutex(SDL::getMutex());

	LOG_DEBUG("GStreamerVideo", "Pipeline unloaded, now in READY state.");

	return true;
}

// Main function to compute perspective transform from 4 arbitrary points
inline std::array<double, 9> computePerspectiveMatrixFromCorners(
	int width,
	int height,
	const std::array<Point2D, 4>& pts)
{
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
		return false;
	}

	// Set playbin flags and properties.
	gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_SOFT_VOLUME;
	g_object_set(playbin_, "flags", flags, "instant-uri", TRUE, "async-handling", TRUE, nullptr);

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
			return false;
		}

		GstElement* videoBin = gst_bin_new("video_bin");
		if (!videoBin) {
			LOG_DEBUG("GStreamerVideo", "Could not create video bin");
			return false;
		}

		gst_bin_add_many(GST_BIN(videoBin), perspective_, videoSink_, nullptr);

		// Link perspective to appsink.
		if (!gst_element_link(perspective_, videoSink_)) {
			LOG_DEBUG("GStreamerVideo", "Could not link perspective to appsink");
			return false;
		}

		// Create a ghost pad to expose the sink pad of the perspective element.
		GstPad* perspectiveSinkPad = gst_element_get_static_pad(perspective_, "sink");
		if (!perspectiveSinkPad) {
			LOG_DEBUG("GStreamerVideo", "Could not get sink pad from perspective element");
			return false;
		}
		GstPad* ghostPad = gst_ghost_pad_new("sink", perspectiveSinkPad);
		gst_object_unref(perspectiveSinkPad);
		if (!gst_element_add_pad(videoBin, ghostPad)) {
			LOG_DEBUG("GStreamerVideo", "Could not add ghost pad to video bin");
			return false;
		}

		// Set the bin as the video-sink.
		g_object_set(playbin_, "video-sink", videoBin, nullptr);
	}
	else {
		// Simple pipeline: set appsink directly as video-sink.
		g_object_set(playbin_, "video-sink", videoSink_, nullptr);
	}

	return true;
}


bool GStreamerVideo::play(const std::string& file) {
	playCount_ = 0;
	if (!initialized_) {
		return false;
	}

	// 1. Create the pipeline if we haven’t already
	if (!createPipelineIfNeeded()) {
		LOG_ERROR("Video", "Failed to create GStreamer pipeline");
		return false;
	}

	// reconnect the pad probe if it's not connected.
	if (GstPad* pad = gst_element_get_static_pad(videoSink_, "sink")) {
		if (padProbeId_ != 0) {
			gst_pad_remove_probe(pad, padProbeId_);
			padProbeId_ = 0;
		}
		padProbeId_ = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
			padProbeCallback, this, nullptr);
		gst_object_unref(pad);
	}

	// Convert file path to URI
	gchar* uriFile = gst_filename_to_uri(file.c_str(), nullptr);
	if (!uriFile) {
		LOG_DEBUG("Video", "Failed to convert filename to URI");
		return false;
	}
	GstState current, pending;
	gst_element_get_state(playbin_, &current, &pending, 0);

	// Update URI - no need to set to READY first
	g_object_set(playbin_, "uri", uriFile, nullptr);
	g_free(uriFile);

	if (current != GST_STATE_PAUSED) {
		GstStateChangeReturn stateRet = gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PAUSED);
		if (stateRet != GST_STATE_CHANGE_ASYNC && stateRet != GST_STATE_CHANGE_SUCCESS) {
			isPlaying_ = false;
			stop();
			return false;
		}
	}

	paused_ = true;
	isPlaying_ = true;
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

	LOG_DEBUG("GStreamerVideo", "Playing file: " + file);

	// Let future calls proceed
	stopping_.store(false, std::memory_order_release);

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

GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
	auto* video = static_cast<GStreamerVideo*>(user_data);

	auto* event = GST_PAD_PROBE_INFO_EVENT(info);
	if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
		GstCaps* caps = nullptr;
		gst_event_parse_caps(event, &caps);
		if (caps) {
			// Create new video info from caps
			GstVideoInfo* newInfo = gst_video_info_new_from_caps(caps);
			if (newInfo) {
				// Store dimensions before freeing old info
				int newWidth = GST_VIDEO_INFO_WIDTH(newInfo);
				int newHeight = GST_VIDEO_INFO_HEIGHT(newInfo);

				// Free old info and update pointer atomically
				if (video->videoInfo_) {
					gst_video_info_free(video->videoInfo_);
				}
				video->videoInfo_ = newInfo;

				if (newWidth > 0 && newHeight > 0) {
					// Update texture validity
					if (video->videoTexture_ &&
						video->textureWidth_ == newWidth &&
						video->textureHeight_ == newHeight) {
						video->textureValid_ = true;
						LOG_DEBUG("GStreamerVideo", "Will reuse existing texture for dimensions " +
							std::to_string(newWidth) + "x" + std::to_string(newHeight));
					}
					else {
						video->textureValid_ = false;
						LOG_DEBUG("GStreamerVideo", "Will create new texture for dimensions " +
							std::to_string(newWidth) + "x" + std::to_string(newHeight));
					}

					video->width_.store(newWidth, std::memory_order_release);
					video->height_.store(newHeight, std::memory_order_release);

					// Perspective box
					if (video->hasPerspective_ && !video->textureValid_) {
						// Only update perspective matrix if texture is invalid.
						// Free and recompute the cached GValueArray if necessary.
						if (video->perspective_gva_) {
							g_value_array_free(video->perspective_gva_);
							video->perspective_gva_ = nullptr;
						}
						std::array<Point2D, 4> perspectiveBox = {
							Point2D(video->perspectiveCorners_[0], video->perspectiveCorners_[1]), // top-left
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

						// Set the new perspective matrix.
						if (video->perspective_) {
							g_object_set(video->perspective_, "matrix", video->perspective_gva_, nullptr);
						}
					}
				}
			}
		}
		gst_pad_remove_probe(pad, video->padProbeId_);
		video->padProbeId_ = 0;
	}
	return GST_PAD_PROBE_OK;
}

void GStreamerVideo::createSdlTexture() {
	int newWidth = width_.load(std::memory_order_acquire);
	int newHeight = height_.load(std::memory_order_acquire);

	// Validate dimensions
	if (newWidth <= 0 || newHeight <= 0) {
		LOG_ERROR("GStreamerVideo", "Invalid dimensions (" +
			std::to_string(newWidth) + "x" + std::to_string(newHeight) + ").");
		textureValid_ = false;
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

		textureValid_ = false;

		// Create YUV texture for video
		videoTexture_ = SDL_CreateTexture(
			SDL::getRenderer(monitor_), sdlFormat_,
			SDL_TEXTUREACCESS_STREAMING, newWidth, newHeight);

		if (!videoTexture_) {
			LOG_ERROR("GStreamerVideo", "SDL_CreateTexture failed: " + std::string(SDL_GetError()));
			textureValid_ = false;
			return;
		}

		SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;
		SDL_SetTextureBlendMode(videoTexture_, blendMode);


		texture_ = videoTexture_;  // Start pointing to video texture

	}

	textureWidth_ = newWidth;
	textureHeight_ = newHeight;
	textureValid_ = true;
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
	return height_;
}

int GStreamerVideo::getWidth() {
	return width_;
}

void GStreamerVideo::draw() {
	if (!isPlaying_ || stopping_.load(std::memory_order_acquire)) {
		return;
	}

	// Try to pull a sample from the appsink.
	GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(videoSink_), 0);
	if (!sample) {
		GstState state;
		gst_element_get_state(GST_ELEMENT(playbin_), &state, nullptr, 0);
		if (state == GST_STATE_PLAYING && gst_app_sink_is_eos(GST_APP_SINK(videoSink_))) {
			if (getCurrent() > GST_SECOND) {
				playCount_++;
				if (!numLoops_ || numLoops_ > playCount_) {
					restart();
				}
				else {
					stop();
				}
			}
		}
		return;
	}

	GstBuffer* buf = gst_sample_get_buffer(sample);
	GstVideoFrame frame;

	SDL_LockMutex(SDL::getMutex());
	if (!videoInfo_) {
		SDL_UnlockMutex(SDL::getMutex());
		gst_sample_unref(sample);
		return;
	}

	if (!gst_video_frame_map(&frame, videoInfo_, buf, GST_MAP_READ)) {
		SDL_UnlockMutex(SDL::getMutex());
		gst_sample_unref(sample);
		return;
	}

	// Check if we need to switch to video texture.
	if (textureValid_ && texture_ != videoTexture_) {
		texture_ = videoTexture_;
	}

	// Handle texture creation if needed.
	if (!textureValid_) {
		createSdlTexture();
	}

	// Update the texture if valid.
	if (textureValid_ && texture_ == videoTexture_) {
		if (sdlFormat_ == SDL_PIXELFORMAT_NV12) {
			SDL_UpdateNVTexture(texture_, nullptr,
				static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
				static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1));
		}
		else if (sdlFormat_ == SDL_PIXELFORMAT_IYUV) {
			SDL_UpdateYUVTexture(texture_, nullptr,
				static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
				static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1),
				static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 2)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2));
		}
		else if (sdlFormat_ == SDL_PIXELFORMAT_ABGR8888) {
			// For RGBA, there is only one plane (plane 0).
			SDL_UpdateTexture(texture_, nullptr,
				static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
		}
	}
	SDL_UnlockMutex(SDL::getMutex());
	gst_video_frame_unmap(&frame);
	gst_sample_unref(sample);
}


bool GStreamerVideo::isPlaying() {
	return isPlaying_;
}

void GStreamerVideo::setVolume(float volume) {
	if (!isPlaying_)
		return;
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
	if (!isPlaying_)
		return;

	if (paused_)
	{
		paused_ = false;
		if (gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
			return;  // Failed to resume, keep state unchanged
	}
	else
	{
		paused_ = true;
		if (gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
			return;  // Failed to pause, keep state unchanged
	}
}

void GStreamerVideo::restart() {
	if (!isPlaying_)
		return;

	// Clear buffered frames
  //  bufferQueue_.clear();


	// Use same seeking method consistently
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
	return paused_;
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