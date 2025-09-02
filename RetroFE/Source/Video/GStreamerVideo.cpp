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
#include "GlibLoop.h"
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
#include <utility>

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

static std::array<double, 9> computePerspectiveMatrixFromCorners(
	int width,
	int height,
	const std::array<Point2D, 4>& pts);

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
		case GST_MESSAGE_ASYNC_START: {
			// Any state transition (or flushing seek) kicked off
			video->pipeLineReady_.store(false, std::memory_order_release);
			break;
		}

		case GST_MESSAGE_STATE_CHANGED: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_)) {
				GstState oldS, newS, pending;
				gst_message_parse_state_changed(msg, &oldS, &newS, &pending);

				// Only publish a settled state
				if (pending == GST_STATE_VOID_PENDING) {
					switch (newS) {
						case GST_STATE_PLAYING:
						video->actualState_.store(IVideo::VideoState::Playing, std::memory_order_release);
						if (video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {
							bool expected = false;
							(void)video->pipeLineReady_.compare_exchange_strong(expected, true,
								std::memory_order_release, std::memory_order_relaxed);
						}
						break;
						case GST_STATE_PAUSED:
						video->actualState_.store(IVideo::VideoState::Paused, std::memory_order_release);
						if (video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {
							bool expected = false;
							(void)video->pipeLineReady_.compare_exchange_strong(expected, true,
								std::memory_order_release, std::memory_order_relaxed);
						}
						break;
						default:
						video->actualState_.store(IVideo::VideoState::None, std::memory_order_release);
						video->pipeLineReady_.store(false, std::memory_order_release);
						// NEW: one-shot notify if armed
						if (video->notifyOnNone_.exchange(false, std::memory_order_acq_rel)) {
							std::function<void(GStreamerVideo*)> cb;
							{ std::lock_guard<std::mutex> g(video->cbMutex_); cb = std::move(video->onBecameNone_); }
							if (cb) cb(video);
						}
						break;
					}
				}
			}
			break;
		}

		case GST_MESSAGE_ASYNC_DONE: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_) &&
				video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {

				// Check that we're actually in a usable state
				GstState current, pending;
				if (gst_element_get_state(video->pipeline_, &current, &pending, 0) == GST_STATE_CHANGE_SUCCESS) {
					if (current == GST_STATE_PAUSED || current == GST_STATE_PLAYING) {
						bool expected = false;
						(void)video->pipeLineReady_.compare_exchange_strong(expected, true,
							std::memory_order_release, std::memory_order_relaxed);
					}
				}
			}
			break;
		}
		case GST_MESSAGE_EOS: {
			// Check if the EOS is from our main playbin element
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_)) {
				uint64_t session = video->currentPlaySessionId_.load();
				LOG_DEBUG("GStreamerVideo", "BusCallback: Received EOS for " + video->currentFile_ +
					" (Session: " + std::to_string(session) + ")");
				if (video->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
					LOG_DEBUG("GStreamerVideo", "EOS ignored (unloaded) for session " + std::to_string(session));
					break;
				}
				if (video->pipeline_ && video->getCurrent() > GST_SECOND / 2) { // Check if it played for a meaningful duration
					video->playCount_++;
					if (!video->numLoops_ || video->numLoops_ > video->playCount_) {
						LOG_DEBUG("GStreamerVideo", "BusCallback: Looping " + video->currentFile_);
						video->loop();
					}
					else {
						LOG_DEBUG("GStreamerVideo", "BusCallback: Finished loops for " + video->currentFile_ + ". Pausing.");
						video->pause(); // Or perhaps a full stop/unload depending on desired behavior
						video->pipeLineReady_.store(false, std::memory_order_release);
					}
				}
				else if (video->pipeline_) {
					LOG_DEBUG("GStreamerVideo", "BusCallback: EOS received very early for " + video->currentFile_ + ". Not looping.");
					// Potentially an issue if EOS comes too fast, could indicate a problem loading the file.
					// video->hasError_.store(true, std::memory_order_release);
					// video->isPlaying_ = false;
				}
			}
			break;
		}
		case GST_MESSAGE_ERROR: {
			GError* err = nullptr; gchar* dbg = nullptr;
			gst_message_parse_error(msg, &err, &dbg);

			const bool unloading = (video->targetState_ == IVideo::VideoState::None);
			if (unloading && err && err->domain == GST_STREAM_ERROR) {
				LOG_DEBUG("GStreamerVideo", std::string("Ignoring stream error during unload: ") +
					(err->message ? err->message : "n/a"));
				if (err) g_error_free(err);
				if (dbg) g_free(dbg);
				break;
			}

			LOG_ERROR("GStreamerVideo", std::string("busCallback(): GStreamer ERROR received for ") +
				video->currentFile_ + ": " +
				(err && err->message ? err->message : "Unknown") +
				(dbg ? (std::string(" | Debug: ") + dbg) : ""));
			video->hasError_.store(true, std::memory_order_release);
			video->pipeLineReady_.store(false, std::memory_order_release);
			video->targetState_.store(IVideo::VideoState::None, std::memory_order_release);
			if (err) g_error_free(err);
			if (dbg) g_free(dbg);
			break;
		}
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

namespace {
	void detachAndDrainSink(GstElement* sink, guint* probeId /*nullable*/) {
		if (!sink || !GST_IS_APP_SINK(sink)) return;

		static GstAppSinkCallbacks kEmpty{};
		gst_app_sink_set_callbacks(GST_APP_SINK(sink), &kEmpty, nullptr, nullptr);

		if (probeId && *probeId != 0) {
			if (GstPad* pad = gst_element_get_static_pad(GST_ELEMENT(sink), "sink")) {
				gst_pad_remove_probe(pad, *probeId);
				gst_object_unref(pad);
			}
			*probeId = 0;
		}

		while (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) gst_sample_unref(s);
		while (GstSample* s = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 0)) gst_sample_unref(s);
	}
}

bool GStreamerVideo::stop() {
	const std::string currentFileForLog = currentFile_;
	LOG_INFO("GStreamerVideo", "Stop (full cleanup) called for " +
		(currentFileForLog.empty() ? "unspecified video" : currentFileForLog));

	// Gate our own state immediately.
	pipeLineReady_.store(false, std::memory_order_release);
	targetState_.store(IVideo::VideoState::None, std::memory_order_release);

	// Disable both sinks first so no new callbacks fire.
	detachAndDrainSink(videoSink_, &padProbeId_);
	guint noProbe = 0;
	detachAndDrainSink(audioSink_, &noProbe);

	// Remove AudioBus source now (no more callbacks will push).
	if (videoSourceId_ != 0) {
		AudioBus::instance().removeSource(videoSourceId_);
		videoSourceId_ = 0;
	}

	// Pipeline teardown
	if (pipeline_) {
		// Stop bus delivery and remove watch
		if (GstBus* bus = gst_element_get_bus(pipeline_)) {
			gst_bus_set_flushing(bus, TRUE);
			gst_object_unref(bus);
		}
		const guint id = std::exchange(busWatchId_, 0);
		if (id != 0) {
			GlibLoop::instance().invokeAndWait([id] {
				if (GMainContext* ctx = g_main_context_get_thread_default()) {
					if (GSource* src = g_main_context_find_source_by_id(ctx, id)) g_source_destroy(src);
				}
				});
		}

		// Flush, step to NULL and wait
		gst_element_send_event(pipeline_, gst_event_new_flush_start());
		gst_element_set_state(pipeline_, GST_STATE_NULL);
		(void)gst_element_get_state(pipeline_, nullptr, nullptr, 2 * GST_SECOND);
		gst_element_send_event(pipeline_, gst_event_new_flush_stop(TRUE));

		// Disconnect element-setup
		if (elementSetupHandlerId_ != 0 && playbin_ &&
			g_signal_handler_is_connected(playbin_, elementSetupHandlerId_)) {
			g_signal_handler_disconnect(playbin_, elementSetupHandlerId_);
			elementSetupHandlerId_ = 0;
		}

		// Now it is safe to free our callback user_data
		if (audioCtx_) { g_free(audioCtx_); audioCtx_ = nullptr; }
		if (videoCtx_) { g_free(videoCtx_); videoCtx_ = nullptr; }

		LOG_DEBUG("GStreamerVideo::stop", "Unreffing pipeline to guarantee final cleanup.");
		gst_object_unref(pipeline_);

		// Clear pointers
		pipeline_ = nullptr;
		playbin_ = nullptr;
		videoSink_ = nullptr;
		audioSink_ = nullptr;
		perspective_ = nullptr;
	}
	else {
		LOG_DEBUG("GStreamerVideo", "stop(): No pipeline_ was active to stop and unref.");
	}

	// SDL textures + staged data
	SDL_LockMutex(SDL::getMutex());
	texture_ = nullptr;
	for (int i = 0; i < 3; ++i) {
		if (videoTexRing_[i]) { SDL_DestroyTexture(videoTexRing_[i]); videoTexRing_[i] = nullptr; }
	}
	SDL_UnlockMutex(SDL::getMutex());

	if (GstSample* s = stagedSample_.exchange(nullptr, std::memory_order_acq_rel)) gst_sample_unref(s);
	if (perspective_gva_) { g_value_array_free(perspective_gva_); perspective_gva_ = nullptr; }

	// Reset members
	width_ = height_ = -1;
	currentFile_.clear();
	playCount_ = 0; numLoops_ = 0;
	volume_ = currentVolume_ = 0.0f;
	lastSetVolume_ = -1.0f; lastSetMuteState_ = true;

	LOG_INFO("GStreamerVideo", "stop(): Instance for " +
		(currentFileForLog.empty() ? "previous video" : currentFileForLog) +
		" fully stopped and all resources released.");
	return true;
}

bool GStreamerVideo::unload() {
	LOG_DEBUG("GStreamerVideo", "Unload (async) called for " + currentFile_);

	if (!playbin_ || actualState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		actualState_.store(IVideo::VideoState::None, std::memory_order_release);
		if (notifyOnNone_.exchange(false, std::memory_order_acq_rel)) {
			std::function<void(GStreamerVideo*)> cb;
			{ std::lock_guard<std::mutex> g(cbMutex_); cb = std::move(onBecameNone_); }
			if (cb) cb(this);
		}
		return true;
	}

	// App-side gates
	pipeLineReady_.store(false, std::memory_order_release);
	targetState_.store(IVideo::VideoState::None, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lk(updateFuncMutex_);
		updateTextureFunc_ = [](SDL_Texture*, GstVideoFrame*) { return false; };
	}
	texture_ = nullptr;

	// Disable both sinks and drain
	detachAndDrainSink(videoSink_, &padProbeId_);
	guint noProbe = 0;
	detachAndDrainSink(audioSink_, &noProbe);

	// Remove (or clear) the AudioBus source so next play() starts fresh
	if (videoSourceId_ != 0) {
		AudioBus::instance().removeSource(videoSourceId_);
		videoSourceId_ = 0;
	}

	// Clear any staged video sample on our side
	if (GstSample* s = stagedSample_.exchange(nullptr, std::memory_order_acq_rel)) gst_sample_unref(s);

	// Drop to READY (we plan to reuse)
	gst_element_set_state(pipeline_, GST_STATE_READY);
	// No blocking wait necessary; bus will catch up shortly.

	// Keep callback ctx pointers for the next play()?
	// We’ll reallocate fresh ctx’s on next play(), so free them now.
	if (audioCtx_) { g_free(audioCtx_); audioCtx_ = nullptr; }
	if (videoCtx_) { g_free(videoCtx_); videoCtx_ = nullptr; }

	// Local bookkeeping for the next play()
	currentFile_ = "[unloading]";
	playCount_ = 0;
	numLoops_ = 0;
	width_ = -1;
	height_ = -1;

	return true;
}



// Function to compute perspective transform from 4 arbitrary points
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
	if (pipeline_) {
		return true;
	}

	// Create playbin3 and appsink elements.
	pipeline_ = gst_pipeline_new(nullptr);
	playbin_ = gst_element_factory_make("playbin3", "player");

	gst_bin_add(GST_BIN(pipeline_), playbin_);

	videoSink_ = gst_element_factory_make("appsink", "video_sink");

	if (!pipeline_ || !playbin_ || !videoSink_) {
		LOG_DEBUG("Video", "Could not create GStreamer elements");
		hasError_.store(true, std::memory_order_release);
		return false;
	}
	audioSink_ = gst_element_factory_make("appsink", "audio_sink");
	if (!audioSink_) {
		LOG_ERROR("GStreamerVideo", "Could not create audio appsink");
		hasError_.store(true, std::memory_order_release);
		return false;
	}
	g_object_set(audioSink_,
		"emit-signals", FALSE,
		"max-buffers", 10,     // small, drop if we fall behind
		"drop", TRUE,
		"sync", TRUE,        // we pace via SDL device, not GST
		"enable-last-sample", FALSE,
		"wait-on-eos", FALSE,           // nicer shutdown
		nullptr);

	GstCaps* acaps = gst_caps_from_string("audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2");
	gst_app_sink_set_caps(GST_APP_SINK(audioSink_), acaps);
	gst_caps_unref(acaps);


	// Force the system clock and disable auto reselection
	GstClock* sys = gst_system_clock_obtain();
	gst_pipeline_use_clock(GST_PIPELINE(pipeline_), sys);
	gst_object_unref(sys);

	// Set playbin flags and properties.
	gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_SOFT_VOLUME;
	g_object_set(playbin_, "flags", flags, "instant-uri", TRUE, nullptr);
	g_object_set(pipeline_, "async-handling", TRUE, nullptr);

	// Configure appsink.
	g_object_set(videoSink_,
		"emit-signals", FALSE,          // fewer wakeups
		"max-buffers", 2,              // tiny queue
		"qos", TRUE,
		"drop", TRUE,           // drop if we fall behind
		"sync", TRUE,
		"enable-last-sample", FALSE,
		NULL);

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
		gst_object_ref_sink(videoBin);
		// Set the bin as the video-sink.
		g_object_set(playbin_, "video-sink", videoBin, nullptr);
		gst_object_unref(videoBin);

		GstPad* sinkPad = gst_element_get_static_pad(videoSink_, "sink");
		if (sinkPad) {
			if (padProbeId_ != 0) { // Remove any existing probe on this pad from this instance
				gst_pad_remove_probe(sinkPad, padProbeId_);
				padProbeId_ = 0;
			}

			auto* probeCtx = static_cast<SessionCtx*>(
				g_malloc0(sizeof(SessionCtx)));
			probeCtx->self = this;
			probeCtx->session = currentPlaySessionId_.load(std::memory_order_acquire);

			padProbeId_ = gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
				&GStreamerVideo::padProbeCallback, probeCtx,
				[](gpointer data) { g_free(data); });

			gst_object_unref(sinkPad);
		}
		else {
			LOG_ERROR("GStreamerVideo", "Failed to get sink pad from videoSink_ for file: " + currentFile_);
			hasError_.store(true);
			return false;
		}

	}
	else {
		// Simple pipeline: set appsink directly as video-sink.
		g_object_set(playbin_, "video-sink", videoSink_, nullptr);
		g_object_set(playbin_, "audio-sink", audioSink_, nullptr);
	}

	if (GstBus* bus = gst_element_get_bus(pipeline_ /* or playbin_ if that’s your top */)) {
		busWatchId_ = GlibLoop::instance().addBusWatch(
			bus,
			+[](GstBus* b, GstMessage* m, gpointer self) -> gboolean {
				return GStreamerVideo::busCallback(b, m, self); // must return TRUE
			},
			this,
			[](gpointer self) { static_cast<GStreamerVideo*>(self)->busWatchId_ = 0; } // only clears when removed
		);
		gst_object_unref(bus);
	}

	return true;
}

void GStreamerVideo::initializeUpdateFunction() {
	const auto session = currentPlaySessionId_.load(std::memory_order_acquire);
	const auto fmt = sdlFormat_;

	auto make_guard = [this, session, fmt](auto impl) {
		return [this, session, fmt, impl](SDL_Texture* tex, GstVideoFrame* frame) -> bool {
			if (currentPlaySessionId_.load(std::memory_order_acquire) != session) return false;
			if (sdlFormat_ != fmt) return false;
			if (!tex || !frame) return false;
			return impl(tex, frame);
			};
		};

	std::function<bool(SDL_Texture*, GstVideoFrame*)> f;
	switch (sdlFormat_) {
		case SDL_PIXELFORMAT_IYUV:     f = make_guard([this](auto t, auto f) { return updateTextureFromFrameIYUV(t, f); }); break;
		case SDL_PIXELFORMAT_NV12:     f = make_guard([this](auto t, auto f) { return updateTextureFromFrameNV12(t, f); }); break;
		case SDL_PIXELFORMAT_ABGR8888: f = make_guard([this](auto t, auto f) { return updateTextureFromFrameRGBA(t, f); }); break;
		default:                       f = [](auto, auto) { return false; }; break;
	}
	std::lock_guard<std::mutex> lk(updateFuncMutex_);
	updateTextureFunc_ = std::move(f);
}

bool GStreamerVideo::play(const std::string& file) {

	if (!initialized_) {
		LOG_ERROR("GStreamerVideo", "Play called but GStreamer not initialized for file: " + file);
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	if (pipeline_) {
		LOG_DEBUG("GStreamerVideo", "Synchronizing pipeline state before play: " + file);
		constexpr GstClockTime syncTimeout = 2 * GST_SECOND;
		GstStateChangeReturn syncRet = gst_element_get_state(pipeline_, nullptr, nullptr, syncTimeout);

		if (syncRet == GST_STATE_CHANGE_ASYNC || syncRet == GST_STATE_CHANGE_FAILURE) {
			LOG_WARNING("GStreamerVideo", "Previous state change pending/failed, bouncing to READY for: " + file);
			gst_element_set_state(pipeline_, GST_STATE_READY);
			(void)gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
		}
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

	initializeUpdateFunction();

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


	// -------------------
	// VIDEO appsink cbs
	// -------------------
	{
		// video
		videoCbs_.new_preroll = &GStreamerVideo::on_new_preroll;
		videoCbs_.new_sample = &GStreamerVideo::on_new_sample;
		videoCtx_ = (SessionCtx*)g_malloc0(sizeof(SessionCtx));
		videoCtx_->self = this;
		videoCtx_->session = currentPlaySessionId_.load(std::memory_order_acquire);
		gst_app_sink_set_callbacks(GST_APP_SINK(videoSink_), &videoCbs_, videoCtx_, nullptr);
	}

	// Ensure an AudioBus source exists before audio callback fires
	if (videoSourceId_ == 0) {
		videoSourceId_ = AudioBus::instance().addSource("video-preview");
	}

	// -------------------
	// AUDIO appsink cbs
	// -------------------
	{
		audioCbs_.new_sample = &GStreamerVideo::on_audio_new_sample;
		audioCtx_ = (SessionCtx*)g_malloc0(sizeof(SessionCtx));
		audioCtx_->self = this;
		audioCtx_->session = currentPlaySessionId_.load(std::memory_order_acquire);
		gst_app_sink_set_callbacks(GST_APP_SINK(audioSink_), &audioCbs_, audioCtx_, nullptr);
	}

	// Request PAUSED (preroll) asynchronously; do not wait here
	GstStateChangeReturn scr = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
	targetState_.store(IVideo::VideoState::Paused, std::memory_order_release);
	if (scr == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Pipeline failed to set PAUSED for " + file);
		pipeLineReady_.store(false, std::memory_order_release);
		hasError_.store(true, std::memory_order_release);
		return false;
	}
	// ASYNC is expected; busCallback will flip flags on PAUSED/PLAYING transitions
	if (scr == GST_STATE_CHANGE_ASYNC)
		LOG_DEBUG("GStreamerVideo", "PAUSED is async for " + file);
	else
		LOG_DEBUG("GStreamerVideo", "PAUSED immediately for " + file);

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

GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
	auto* ctx = static_cast<SessionCtx*>(user_data);
	auto* self = ctx ? ctx->self : nullptr;
	if (!self || !self->isCurrentSession(ctx->session)) {
		return GST_PAD_PROBE_REMOVE;  // stale instance
	}
	if (!self->playbin_) return GST_PAD_PROBE_REMOVE;

	GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
	if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
		GstCaps* caps = nullptr;
		gst_event_parse_caps(ev, &caps);
		if (!caps) return GST_PAD_PROBE_REMOVE;

		const GstStructure* s = gst_caps_get_structure(caps, 0);
		int w = 0, h = 0;
		if (gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h) && w > 0 && h > 0) {
			if (self->hasPerspective_) {
				if (w != self->lastPerspectiveW_ || h != self->lastPerspectiveH_) {
					self->lastPerspectiveW_ = w; self->lastPerspectiveH_ = h;

					struct Task { GStreamerVideo* self; int w, h; uint64_t session; };
					auto* task = g_new0(Task, 1);
					task->self = self;
					task->w = w;
					task->h = h;
					task->session = ctx->session;

					// run on the main loop, with a destroy notify to avoid leaks if never dispatched
					g_main_context_invoke_full(
						/*context*/ GlibLoop::instance().context(),
						/*priority*/ G_PRIORITY_DEFAULT,
						[](gpointer data) -> gboolean {
							Task* t = static_cast<Task*>(data);
							auto* v = t->self;
							if (!v) return G_SOURCE_REMOVE;

							// Still the current session?
							if (v->currentPlaySessionId_.load(std::memory_order_acquire) != t->session)
								return G_SOURCE_REMOVE;
							if (!v->perspective_) return G_SOURCE_REMOVE;

							// Compute/apply matrix on main thread
							std::array<Point2D, 4> box = {
								Point2D(double(v->perspectiveCorners_[0]), double(v->perspectiveCorners_[1])),
								Point2D(double(v->perspectiveCorners_[2]), double(v->perspectiveCorners_[3])),
								Point2D(double(v->perspectiveCorners_[4]), double(v->perspectiveCorners_[5])),
								Point2D(double(v->perspectiveCorners_[6]), double(v->perspectiveCorners_[7]))
							};
							const auto mat = computePerspectiveMatrixFromCorners(t->w, t->h, box);

							if (v->perspective_gva_) { g_value_array_free(v->perspective_gva_); v->perspective_gva_ = nullptr; }
							v->perspective_gva_ = g_value_array_new(9);

							GValue val = G_VALUE_INIT; g_value_init(&val, G_TYPE_DOUBLE);
							for (double e : mat) { g_value_set_double(&val, e); g_value_array_append(v->perspective_gva_, &val); }
							g_value_unset(&val);

							g_object_set(G_OBJECT(v->perspective_), "matrix", v->perspective_gva_, nullptr);
							return G_SOURCE_REMOVE;
						},
						/*data*/ task,
						/*destroy_notify*/ [](gpointer data) { g_free(data); }
					);

					self->padProbeId_ = 0;
				}
			}
		}
		return GST_PAD_PROBE_REMOVE;
	}
	return GST_PAD_PROBE_OK;
}

void GStreamerVideo::elementSetupCallback([[maybe_unused]] GstElement* playbin,
	GstElement* element,
	[[maybe_unused]] gpointer data) {
	const gchar* name = gst_element_get_name(element);

	auto has_prop = [](GstElement* e, const char* p) {
		return g_object_class_find_property(G_OBJECT_GET_CLASS(e), p) != nullptr;
		};

	// ---- Tune multiqueue to reduce CPU churn ----
	if (g_str_has_prefix(name, "multiqueue")) {
		if (has_prop(element, "max-size-buffers")) g_object_set(element, "max-size-buffers", 2, NULL);
		if (has_prop(element, "max-size-bytes"))   g_object_set(element, "max-size-bytes", (guint64)0, NULL);
		if (has_prop(element, "max-size-time"))    g_object_set(element, "max-size-time", (guint64)0, NULL);
		if (has_prop(element, "low-percent"))      g_object_set(element, "low-percent", 5, NULL);
		if (has_prop(element, "high-percent"))     g_object_set(element, "high-percent", 25, NULL);
		if (has_prop(element, "sync-by-running-time"))
			g_object_set(element, "sync-by-running-time", TRUE, NULL);
	}

	// ---- Tune plain queues (if present) ----
	if (g_str_has_prefix(name, "vqueue")) {
		if (has_prop(element, "max-size-buffers")) g_object_set(element, "max-size-buffers", 2, NULL);
		if (has_prop(element, "silent"))           g_object_set(element, "silent", TRUE, NULL);
	}

	// ---- Video decoder settings ----
	if (!Configuration::HardwareVideoAccel && GST_IS_VIDEO_DECODER(element)) {
		g_object_set(element,
			"thread-type", Configuration::AvdecThreadType,
			"max-threads", Configuration::AvdecMaxThreads,
			"direct-rendering", FALSE,
			"std-compliance", 0,
			nullptr);
	}
}


GstFlowReturn GStreamerVideo::on_new_preroll(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<SessionCtx*>(user_data);
	if (!ctx || !ctx->self) return GST_FLOW_OK;
	auto* self = ctx->self;
	if (!self->isCurrentSession(ctx->session)) return GST_FLOW_OK;

	GstSample* preroll = gst_app_sink_try_pull_preroll(sink, 0);
	if (!preroll) return GST_FLOW_OK;

	if (GstSample* old = self->stagedSample_.exchange(preroll, std::memory_order_acq_rel))
		gst_sample_unref(old);

	return GST_FLOW_OK;
}


GstFlowReturn GStreamerVideo::on_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<SessionCtx*>(user_data);
	if (!ctx || !ctx->self) return GST_FLOW_OK;
	auto* self = ctx->self;
	if (!self->isCurrentSession(ctx->session)) return GST_FLOW_OK;

	if (self->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::Playing)
		return GST_FLOW_OK;

	GstSample* s = gst_app_sink_try_pull_sample(sink, 0);
	if (!s) return GST_FLOW_OK;

	if (GstSample* old = self->stagedSample_.exchange(s, std::memory_order_acq_rel))
		gst_sample_unref(old);

	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_audio_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<SessionCtx*>(user_data);
	if (!ctx || !ctx->self) return GST_FLOW_OK;
	auto* self = ctx->self;
	if (!self->isCurrentSession(ctx->session)) return GST_FLOW_OK;

	if (!self || self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None)
		return GST_FLOW_OK;

	// Pull the sample inside the callback
	GstSample* s = gst_app_sink_pull_sample(sink);
	if (!s) return GST_FLOW_OK;

	if (GstBuffer* b = gst_sample_get_buffer(s)) {
		GstMapInfo mi{};
		if (gst_buffer_map(b, &mi, GST_MAP_READ)) {
			// Fast path: push straight into AudioBus (thread-safe and quick)
			AudioBus::instance().push(self->videoSourceId_, mi.data, (int)mi.size);
			gst_buffer_unmap(b, &mi);
		}
	}
	gst_sample_unref(s);
	return GST_FLOW_OK;
}

void GStreamerVideo::createSdlTexture() {
	SDL_LockMutex(SDL::getMutex());

	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None || !playbin_) {
		SDL_UnlockMutex(SDL::getMutex());
		return;
	}

	if (width_ <= 0 || height_ <= 0) {
		// destroy any existing ring textures
		for (int i = 0; i < 3; i++) {
			if (videoTexRing_[i]) { SDL_DestroyTexture(videoTexRing_[i]); videoTexRing_[i] = nullptr; }
		}
		videoDrawIdx_ = -1;
		SDL_UnlockMutex(SDL::getMutex());
		return;
	}

	// Check if any slot mismatches expected size/format
	bool needsRecreate = false;
	for (int i = 0; i < videoRingCount_; ++i) {
		if (!videoTexRing_[i]) { needsRecreate = true; break; }
		int texW = 0, texH = 0, access = 0; Uint32 fmt = 0;
		if (SDL_QueryTexture(videoTexRing_[i], &fmt, &access, &texW, &texH) != 0 ||
			texW != width_ || texH != height_ || fmt != sdlFormat_ || access != SDL_TEXTUREACCESS_STREAMING) {
			needsRecreate = true; break;
		}
	}

	if (needsRecreate) {
		// Destroy old
		for (int i = 0; i < 3; i++) {
			if (videoTexRing_[i]) { SDL_DestroyTexture(videoTexRing_[i]); videoTexRing_[i] = nullptr; }
		}

		LOG_INFO("GStreamerVideo", "Creating/recreating SDL video texture RING: " +
			std::to_string(width_) + "x" + std::to_string(height_) +
			" fmt=" + std::string(SDL_GetPixelFormatName(sdlFormat_)) +
			" slots=" + std::to_string(videoRingCount_));

		// Create ring
		for (int i = 0; i < videoRingCount_; ++i) {
			SDL_Texture* t = SDL_CreateTexture(
				SDL::getRenderer(monitor_), sdlFormat_, SDL_TEXTUREACCESS_STREAMING, width_, height_);
			if (!t) {
				LOG_ERROR("GStreamerVideo", std::string("SDL_CreateTexture failed: ") + SDL_GetError());
				// Best-effort cleanup
				for (int k = 0; k < 3; k++) { if (videoTexRing_[k]) { SDL_DestroyTexture(videoTexRing_[k]); videoTexRing_[k] = nullptr; } }
				SDL_UnlockMutex(SDL::getMutex());
				return;
			}
			// Make the texture as cheap as possible
			SDL_SetTextureBlendMode(t, softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND);
			SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);

			videoTexRing_[i] = t;
		}

		videoWriteIdx_ = 0;
		videoDrawIdx_ = -1; // publish on first good frame
	}

	SDL_UnlockMutex(SDL::getMutex());
}

void GStreamerVideo::volumeUpdate() {
	if (!pipeLineReady_.load(std::memory_order_acquire) || !playbin_)
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

void GStreamerVideo::draw() {
	GstSample* sample = stagedSample_.exchange(nullptr, std::memory_order_acq_rel);
	if (!sample) return;

	GstBuffer* buf = gst_sample_get_buffer(sample);
	const GstCaps* caps = gst_sample_get_caps(sample);
	if (!buf || !caps) { gst_sample_unref(sample); return; }

	GstVideoInfo info;
	if (!gst_video_info_from_caps(&info, caps)) { gst_sample_unref(sample); return; }

	const int frameW = GST_VIDEO_INFO_WIDTH(&info);
	const int frameH = GST_VIDEO_INFO_HEIGHT(&info);
	if (frameW <= 0 || frameH <= 0) { gst_sample_unref(sample); return; }

	if (frameW != width_ || frameH != height_) {
		width_ = frameW; height_ = frameH;
		createSdlTexture(); // recreates ring if needed
	}

	GstVideoFrame frame;
	if (!gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
		gst_sample_unref(sample); return;
	}

	// Snapshot the updater
	std::function<bool(SDL_Texture*, GstVideoFrame*)> updater;
	{
		std::lock_guard<std::mutex> lk(updateFuncMutex_);
		updater = updateTextureFunc_;
	}

	bool ok = false;
	SDL_LockMutex(SDL::getMutex());

	// Pick a write slot (avoid currently drawn slot if possible)
	int write = videoWriteIdx_;
	if (write == videoDrawIdx_) write = (write + 1) % videoRingCount_;
	SDL_Texture* t = videoTexRing_[write];

	if (t && updater) ok = updater(t, &frame);

	if (ok) {
		// Publish for this frame’s render
		videoDrawIdx_ = write;
		texture_ = t;
		// Advance write for next frame
		videoWriteIdx_ = (write + 1) % videoRingCount_;
	}
	else {
		texture_ = nullptr;
	}

	SDL_UnlockMutex(SDL::getMutex());

	gst_video_frame_unmap(&frame);
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

int GStreamerVideo::getHeight() {
	return height_;
}

int GStreamerVideo::getWidth() {
	return width_;
}

bool GStreamerVideo::isPlaying() {
	return pipeLineReady_.load(std::memory_order_acquire) && actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Playing;
}

void GStreamerVideo::setVolume(float volume) {
	volume_ = volume;
}

void GStreamerVideo::skipForward() {
	if (!pipeLineReady_.load(std::memory_order_acquire))
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
	if (!pipeLineReady_.load(std::memory_order_acquire))
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
	if (!pipeLineReady_.load(std::memory_order_acquire))
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
	if (!pipeLineReady_.load(std::memory_order_acquire))
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
	if (!pipeLineReady_.load(std::memory_order_acquire))
		return;
	// Only return early if actual state is already paused
	if (actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Paused)
		return;
	// Already requested pause?
	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::Paused)
		return;

	targetState_.store(IVideo::VideoState::Paused, std::memory_order_release);

	LOG_DEBUG("GStreamerVideo", "Requesting PAUSED for " + currentFile_);
	if (gst_element_set_state(pipeline_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PAUSED for " + currentFile_);
		hasError_.store(true);
	}
}

void GStreamerVideo::resume() {
	if (!pipeLineReady_.load(std::memory_order_acquire))
		return;
	// Only return early if actual state is already playing
	if (actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Playing)
		return;
	// Already requested play?
	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::Playing)
		return;

	targetState_.store(IVideo::VideoState::Playing, std::memory_order_release);
	LOG_DEBUG("GStreamerVideo", "Requesting PLAYING for " + currentFile_);
	if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PLAYING for " + currentFile_);
		hasError_.store(true);
	}
}

void GStreamerVideo::restart() {
	if (!pipeLineReady_.load(std::memory_order_acquire))
		return;
	bool ok = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH,
		GST_SEEK_TYPE_SET, 0,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	if (!ok) {
		LOG_ERROR("GStreamerVideo", "Failed to seek to start (" + currentFile_ + ")");
	}
}

void GStreamerVideo::loop() {
	if (!pipeLineReady_.load(std::memory_order_acquire))
		return;
	bool ok = gst_element_seek(playbin_, 1.0, GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH,
		GST_SEEK_TYPE_SET, 0,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	if (!ok) {
		LOG_ERROR("GStreamerVideo", "Failed to seek to start (" + currentFile_ + ")");
	}
}

unsigned long long GStreamerVideo::getCurrent() {
	gint64 ret = 0;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &ret) || !pipeLineReady_.load(std::memory_order_acquire))
		ret = 0;
	return (unsigned long long)ret;
}

unsigned long long GStreamerVideo::getDuration() {
	gint64 ret = 0;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &ret) || !pipeLineReady_.load(std::memory_order_acquire))
		ret = 0;
	return (unsigned long long)ret;
}

bool GStreamerVideo::isPaused() {
	return pipeLineReady_.load(std::memory_order_acquire) && actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Paused;
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