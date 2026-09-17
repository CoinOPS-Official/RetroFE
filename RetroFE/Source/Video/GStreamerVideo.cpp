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
#include "../Utility/ThreadPool.h"
#include "../Utility/Utils.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gst/audio/audio.h>
#include <gst/gstdebugutils.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
#include <memory>
#include <cstdint>

bool GStreamerVideo::initialized_ = false;
bool GStreamerVideo::pluginsInitialized_ = false;

// Initialize the static Epoch ID generator
std::atomic<uint64_t> GStreamerVideo::nextUniquePlaybackEpoch_{ 1 };

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

// Utility: SDL_AudioFormat -> GStreamer audio/x-raw format string
static const char* sdl_to_gst_fmt(Uint16 fmt) {
	switch (fmt) {
		case SDL_AUDIO_U8: return "U8";
		case SDL_AUDIO_S8: return "S8";
		case SDL_AUDIO_S16LE: return "S16LE";
		case SDL_AUDIO_S16BE: return "S16BE";
		case SDL_AUDIO_S32LE: return "S32LE";
		case SDL_AUDIO_S32BE: return "S32BE";
		case SDL_AUDIO_F32LE: return "F32LE";
		case SDL_AUDIO_F32BE: return "F32BE";
		default: return nullptr;
	}
}

GStreamerVideo::CallbackCtx* GStreamerVideo::cbCtxRef(CallbackCtx* c) {
	if (!c) return nullptr;
	g_ref_count_inc(&c->ref);
	return c;
}

void GStreamerVideo::cbCtxUnref(gpointer data) {
	auto* c = static_cast<CallbackCtx*>(data);
	if (g_ref_count_dec(&c->ref)) {
		delete c;
	}
}

GStreamerVideo::GStreamerVideo(int monitor)
	: monitor_(monitor) {
	initialize();
	initializePlugins();
}

GStreamerVideo::~GStreamerVideo() {
	// Detach callbacks from this instance immediately
	if (cbCtx_) {
		cbCtx_->self.store(std::weak_ptr<GStreamerVideo>());
	}
	try {
		stop();
	}
	catch (...) {
		LOG_ERROR("GStreamerVideo", "Exception in destructor during stop()");
	}

	// Callback registrations own separate references released by their
	// destroy notifications. Release this instance's original reference last.
	CallbackCtx* ctx = std::exchange(cbCtx_, nullptr);
	if (ctx) {
		cbCtxUnref(ctx);
	}
}

gboolean GStreamerVideo::busCallback(
	GstBus*,
	GstMessage* msg,
	gpointer user_data) {
	auto* ctx =
		static_cast<CallbackCtx*>(user_data);

	if (!ctx)
		return TRUE;

	auto video =
		ctx->self.load().lock();

	if (!video || !video->pipeline_)
		return TRUE;

	const uint64_t epoch =
		ctx->epoch.load(
			std::memory_order_acquire);

	if (epoch !=
		video->playbackEpoch_.load(
			std::memory_order_acquire))
	{
		return TRUE;
	}

	const bool fromPipeline =
		GST_MESSAGE_SRC(msg) ==
		GST_OBJECT(video->pipeline_);

	switch (GST_MESSAGE_TYPE(msg)) {

		case GST_MESSAGE_STATE_CHANGED: {
			if (fromPipeline) {
				GstState oldState;
				GstState newState;
				GstState pendingState;

				gst_message_parse_state_changed(
					msg,
					&oldState,
					&newState,
					&pendingState);

				video->actualGstState_.store(
					newState,
					std::memory_order_release);

				// unload() is deliberately non-blocking. Once the retained
				// playbin3 has actually settled in PAUSED (preferred) or READY
				// (fallback), publish Idle so VideoPool may reuse it.
				if (pendingState == GST_STATE_VOID_PENDING &&
					video->lifecycle_.load(std::memory_order_acquire) ==
						PipelineLifecycle::Draining &&
					(newState == GST_STATE_PAUSED ||
					 newState == GST_STATE_READY))
				{
					video->lifecycle_.store(
						PipelineLifecycle::Idle,
						std::memory_order_release);

					LOG_DEBUG(
						"GStreamerVideo",
						std::string("Retained pipeline settled asynchronously in ") +
						(newState == GST_STATE_PAUSED ? "PAUSED." : "READY."));
				}
			}

			break;
		}

		case GST_MESSAGE_ASYNC_DONE: {
			// ASYNC_DONE only completes the initial preroll contract.
			// A late ASYNC_DONE generated while unload() is draining must
			// never resurrect the object into Ready.
			if (!fromPipeline)
				break;

			if (video->lifecycle_.load(
				std::memory_order_acquire) !=
				PipelineLifecycle::Starting)
			{
				break;
			}

			if (!video->awaitingInitialPreroll_.exchange(
				false,
				std::memory_order_acq_rel))
			{
				break;
			}

			// playbin3 does not expose legacy playbin's n-video property. If a
			// video preroll/sample has already crossed appsink, that callback has
			// set hasVideoStream_. Otherwise this ASYNC_DONE path primarily serves
			// audio-only/cold-start completion.
			video->hasVideoStream_.store(
				video->startupSampleNs_.load(std::memory_order_acquire) != 0,
				std::memory_order_release);

			// Recheck after touching the pipeline. unload() may have begun
			// while this callback was executing.
			if (video->lifecycle_.load(
				std::memory_order_acquire) ==
				PipelineLifecycle::Starting)
			{
				video->lifecycle_.store(
					PipelineLifecycle::Ready,
					std::memory_order_release);
			}

			break;
		}

		case GST_MESSAGE_EOS: {
			if (!fromPipeline)
				break;

			// EOS belongs to active playback only. In particular, never
			// seek/pause/change lifecycle while unload() is draining.
			if (video->lifecycle_.load(
				std::memory_order_acquire) !=
				PipelineLifecycle::Ready)
			{
				break;
			}

			if (video->pipeline_ &&
				video->getCurrent() > GST_SECOND / 2)
			{
				video->playCount_++;

				if (!video->numLoops_ ||
					video->numLoops_ > video->playCount_)
				{
					video->loop();
				}
				else {
					video->loopsFinished_.store(
						true,
						std::memory_order_release);

					video->lifecycle_.store(
						PipelineLifecycle::Idle,
						std::memory_order_release);

					video->playbackState_.store(
						PlaybackState::Paused,
						std::memory_order_release);

					gst_element_set_state(
						video->pipeline_,
						GST_STATE_PAUSED);

					video->playCount_ = 0;
				}
			}
			else if (video->pipeline_) {
				video->loopsFinished_.store(
					true,
					std::memory_order_release);

				video->lifecycle_.store(
					PipelineLifecycle::Idle,
					std::memory_order_release);

				video->playbackState_.store(
					PlaybackState::Paused,
					std::memory_order_release);

				gst_element_set_state(
					video->pipeline_,
					GST_STATE_PAUSED);

				video->playCount_ = 0;
			}

			break;
		}

		case GST_MESSAGE_ERROR: {
			// Errors generated as a consequence of tearing the old
			// pipeline down must not overwrite Draining with Failed or
			// Starting.
			if (video->lifecycle_.load(
				std::memory_order_acquire) ==
				PipelineLifecycle::Draining)
			{
				GError* err = nullptr;
				gchar* dbg = nullptr;

				gst_message_parse_error(
					msg,
					&err,
					&dbg);

				if (err) {
					LOG_DEBUG(
						"GStreamerVideo",
						std::string(
							"Ignoring pipeline error while draining: ") +
						err->message);

					g_error_free(err);
				}

				if (dbg)
					g_free(dbg);

				break;
			}

			const bool retryGL =
				video->glPipelineActive_.exchange(false);

			video->playbackState_.store(
				PlaybackState::None,
				std::memory_order_release);

			// Release whatever token is currently held because the
			// pipeline has failed.
			if (video->pipeline_) {
				gst_element_set_state(
					video->pipeline_,
					GST_STATE_READY);
			}

			GError* err = nullptr;
			gchar* dbg = nullptr;

			gst_message_parse_error(
				msg,
				&err,
				&dbg);

			if (err) {
				LOG_ERROR(
					"GStreamerVideo",
					std::string(
						"GStreamer pipeline error: ") +
					err->message);

				g_error_free(err);
			}

			if (dbg)
				g_free(dbg);

			video->lifecycle_.store(
				retryGL
				? PipelineLifecycle::Starting
				: PipelineLifecycle::Failed,
				std::memory_order_release);

			if (retryGL) {
				LOG_WARNING(
					"GStreamerVideo",
					"GL pipeline failed; retrying this instance "
					"with CPU texture upload");

				video->pendingCpuFallback_.store(
					true,
					std::memory_order_release);
			}

			break;
		}

		default:
		break;
	}

	return TRUE;
}

void GStreamerVideo::initializePlugins() {
	if (!pluginsInitialized_)
	{
		pluginsInitialized_ = true;

#if defined(WIN32)
		enablePlugin("directsoundsink");
		disablePlugin("mfdeviceprovider");

		// Keep NVIDIA-specific decoders out of playbin autoplugging.
		disablePlugin("nvh264dec");
		disablePlugin("nvh265dec");

		if (Configuration::HardwareVideoAccel)
		{
			const std::string rendererBackend =
				SDL::getRendererBackend(0);

			if (rendererBackend == "direct3d11")
			{
				for (const char* codec :
					{ "h264", "h265", "vp8", "vp9", "mpeg2", "av1" })
				{
					enablePlugin(
						std::string("d3d11") + codec + "dec");

					disablePlugin(
						std::string("d3d12") + codec + "dec");
				}

				// Do not let Intel QSV win autoplugging when our renderer and
				// zero-copy interop path are explicitly D3D11.
				disablePlugin("qsvh264dec");
				disablePlugin("qsvh265dec");

				LOG_INFO(
					"GStreamerVideo",
					"D3D11 hardware decoding requested; awaiting frame verification");
			}
			else
			{
				// Non-D3D11 Windows renderer fallback.
				enablePlugin("d3d12h264dec");
				enablePlugin("d3d12h265dec");

				disablePlugin("d3d11h264dec");
				disablePlugin("d3d11h265dec");

				disablePlugin("qsvh264dec");
				disablePlugin("qsvh265dec");

				LOG_INFO(
					"GStreamerVideo",
					"D3D12 hardware decoding requested; awaiting frame verification");
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

			LOG_DEBUG(
				"GStreamerVideo",
				"Using avdec_h264/avdec_h265 for software decoding");
		}

#elif defined(__APPLE__)

		if (!Configuration::HardwareVideoAccel)
		{
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");

			LOG_DEBUG(
				"GStreamerVideo",
				"Using avdec_h264/avdec_h265 for software decoding");
		}

#else

		if (Configuration::HardwareVideoAccel)
		{
			enablePlugin("vah264dec");
			enablePlugin("vah265dec");
		}
		else
		{
			disablePlugin("vah264dec");
			disablePlugin("vah265dec");

			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
		}

#endif
	}
}

void GStreamerVideo::setNumLoops(int n) {
	if (n > 0)
		numLoops_.store(n, std::memory_order_release);
}

SDL_Texture* GStreamerVideo::getTexture() const {
	if (!isTextureReady_) return nullptr;
	return texture_;
}

bool GStreamerVideo::initialize() {
	if (initialized_) return true;
	if (!gst_is_initialized())
	{
		LOG_DEBUG("GStreamer", "Initializing in instance");
		gst_init(nullptr, nullptr);
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

void GStreamerVideo::destroyTextures() {
	if (texture_ == gpuTexture_) texture_ = nullptr;
	gpuTexture_ = nullptr;
	if (texture_) {
		SDL_Texture* t = texture_;
		SDL_RunOnMainThread([](void* data) {
			SDL_DestroyTexture(static_cast<SDL_Texture*>(data));
			}, t, false);
		texture_ = nullptr;
	}
	isTextureReady_ = false;
	allocatedWidth_ = 0;
	allocatedHeight_ = 0;
	allocatedFormat_ = SDL_PIXELFORMAT_UNKNOWN;
}

bool GStreamerVideo::stop() {
	if (gpuInterop_) {
		gpuInterop_->discardFrames();
	}
	glPipelineActive_.store(false);
	pendingCpuFallback_.store(false);
	const uint64_t deadEpoch = playbackEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (cbCtx_) {
		cbCtx_->epoch.store(deadEpoch, std::memory_order_release);
	}
	lifecycle_.store(PipelineLifecycle::Idle, std::memory_order_release);
	playbackState_.store(PlaybackState::None, std::memory_order_release);

	GstElement* pipeline = std::exchange(pipeline_, nullptr);
	instantUriEnabled_ = false;
	GstElement* videoSink = std::exchange(videoSink_, nullptr);
	GstElement* audioSink = std::exchange(audioSink_, nullptr);
	guint busWatchId = std::exchange(busWatchId_, 0);

	if (pipeline && elementSetupHandlerId_ > 0) {
		g_signal_handler_disconnect(pipeline, elementSetupHandlerId_);
		elementSetupHandlerId_ = 0;
	}

	if (pipeline) {
		if (GstBus* bus = gst_element_get_bus(pipeline)) {
			gst_bus_set_flushing(bus, TRUE);
			gst_object_unref(bus);
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
	}

	if (busWatchId != 0) {
		GlibLoop::instance().removeSource(busWatchId);
	}

	if (videoSink) detachAndDrainSink(videoSink, &padProbeId_);
	if (audioSink) detachAndDrainSink(audioSink, nullptr);

	if (videoSourceId_ != 0) {
		AudioBus::instance().setGain(audioHandle_, 0.0f);
		AudioBus::instance().removeSource(videoSourceId_);
		videoSourceId_ = 0;
		audioHandle_.reset();
	}

	if (pipeline) {
		gst_object_unref(pipeline);
	}

	destroyTextures();
	gpuInterop_.reset();

	{
		std::lock_guard<std::mutex> lock(sampleMutex_);
		if (stagedSample_.sample) {
			gst_sample_unref(stagedSample_.sample);
			stagedSample_.sample = nullptr;
		}
		stagedSample_.epoch = 0;
	}

	if (perspective_gva_) {
		g_value_array_free(perspective_gva_);
		perspective_gva_ = nullptr;
	}

	return true;
}

bool GStreamerVideo::isReadyForReuse() const {
	if (!pipeline_)
		return true;

	// The bus callback publishes Idle only after the retained playbin3 has
	// actually settled in PAUSED (preferred) or READY (fallback). Never wait
	// for a GStreamer state transition on the caller/render thread.
	return lifecycle_.load(std::memory_order_acquire) ==
		PipelineLifecycle::Idle;
}

bool GStreamerVideo::prepareForRetarget() {
    // This path is for an instance that is still owned by a VideoComponent.
    // It must NOT enter VideoPool::draining; we only need to quiesce the old
    // URI strongly enough that no old appsink frame can be mistaken for the
    // next URI.

    pendingCpuFallback_.store(false, std::memory_order_release);

    if (!initialized_)
        return false;

    if (!pipeline_)
        return true;

    if (gpuInterop_) {
        gpuInterop_->discardFrames();
    }

    // First stop the old stream from producing more frames. Do this while the
    // old epoch is still authoritative; once PAUSED settles, the appsink queue
    // becomes finite and can be drained deterministically.
    const GstStateChangeReturn setRet =
        gst_element_set_state(pipeline_, GST_STATE_PAUSED);

    if (setRet == GST_STATE_CHANGE_FAILURE) {
        LOG_WARNING(
            "GStreamerVideo",
            "prepareForRetarget(): failed to request PAUSED.");
        return false;
    }

    const Uint64 waitStartNs = SDL_GetTicksNS();

    const GstStateChangeReturn waitRet =
        gst_element_get_state(
            pipeline_,
            nullptr,
            nullptr,
            static_cast<GstClockTime>(500 * GST_MSECOND));

    const double waitMs =
        static_cast<double>(SDL_GetTicksNS() - waitStartNs) / 1000000.0;

    if (waitMs >= 2.0) {
        std::ostringstream ss;
        ss << std::fixed
           << std::setprecision(3)
           << "prepareForRetarget get_state_ms="
           << waitMs
           << " result="
           << static_cast<int>(waitRet);

        LOG_DEBUG("GStreamerPerf", ss.str());
    }

    if (waitRet == GST_STATE_CHANGE_FAILURE ||
        waitRet == GST_STATE_CHANGE_ASYNC)
    {
        LOG_WARNING(
            "GStreamerVideo",
            "prepareForRetarget(): pipeline did not settle in PAUSED.");
        return false;
    }

    // Invalidate every callback that already belongs to the old URI.
    // Importantly, this happens only AFTER the old stream is quiescent.
    const uint64_t deadEpoch =
        playbackEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (cbCtx_) {
        cbCtx_->epoch.store(deadEpoch, std::memory_order_release);
    }

    awaitingInitialPreroll_.store(false, std::memory_order_release);
    playbackState_.store(
        PlaybackState::Paused,
        std::memory_order_release);

    actualGstState_.store(
        GST_STATE_PAUSED,
        std::memory_order_release);

    isTextureReady_ = false;

    dimensions_.store(
        { -1, -1 },
        std::memory_order_release);

    loopsFinished_.store(
        false,
        std::memory_order_release);

    // Drain samples that were queued by the old URI. We deliberately do NOT
    // detach callbacks or remove the pad probe; this is an in-place retarget,
    // not teardown.
    auto drainSink = [](GstElement* sink) {
        if (!sink || !GST_IS_APP_SINK(sink))
            return;

        while (GstSample* s =
            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0))
        {
            gst_sample_unref(s);
        }

        while (GstSample* s =
            gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 0))
        {
            gst_sample_unref(s);
        }
    };

    drainSink(videoSink_);
    drainSink(audioSink_);

    // A callback may already have crossed the appsink boundary before PAUSED
    // settled. Drop that staged frame as well.
    {
        std::lock_guard<std::mutex> lock(sampleMutex_);

        if (stagedSample_.sample) {
            gst_sample_unref(stagedSample_.sample);
            stagedSample_.sample = nullptr;
        }

        stagedSample_.epoch = 0;
    }

    if (videoSourceId_ != 0) {
        AudioBus::instance().setGain(audioHandle_, 0.0f);
        AudioBus::instance().clear(audioHandle_);
    }

    LOG_DEBUG(
        "GStreamerVideo",
        "Prepared active pipeline for clean instant-uri retarget.");

    return true;
}

bool GStreamerVideo::unload() {
	pendingCpuFallback_.store(false, std::memory_order_release);

	if (gpuInterop_) {
		gpuInterop_->discardFrames();
	}

	if (!initialized_)
		return false;

	const PipelineLifecycle life =
		lifecycle_.load(std::memory_order_acquire);

	// A second release request while the retained pipeline is already
	// settling has nothing useful to do. Most importantly, do not wait.
	if (life == PipelineLifecycle::Draining)
		return true;

	// Invalidate every callback/sample belonging to the media item that is
	// being returned to the pool. The playbin3 itself is deliberately kept
	// alive and, when possible, PAUSED so decodebin3 can reuse compatible
	// decoder elements on the next instant URI switch.
	const uint64_t deadEpoch =
		playbackEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;

	if (cbCtx_) {
		cbCtx_->epoch.store(deadEpoch, std::memory_order_release);
	}

	awaitingInitialPreroll_.store(false, std::memory_order_release);

	lifecycle_.store(
		PipelineLifecycle::Draining,
		std::memory_order_release);
	playbackState_.store(
		PlaybackState::None,
		std::memory_order_release);

	isTextureReady_ = false;

	dimensions_.store(
		{ -1, -1 },
		std::memory_order_release);

	currentFile_.clear();

	loopsFinished_.store(
		false,
		std::memory_order_release);

	// Move the staged sample release off the caller/render thread. We have
	// already invalidated its epoch and it is not an SDL-visible GPU frame.
	GstSample* stagedToRelease = nullptr;
	{
		std::lock_guard<std::mutex> lock(sampleMutex_);
		stagedToRelease = stagedSample_.sample;
		stagedSample_.sample = nullptr;
		stagedSample_.epoch = 0;
	}

	if (stagedToRelease) {
		ThreadPool::getInstance().enqueue(
			[stagedToRelease]() {
				gst_sample_unref(stagedToRelease);
			});
	}

	if (videoSourceId_ != 0) {
		AudioBus::instance().setGain(audioHandle_, 0.0f);
		AudioBus::instance().clear(audioHandle_);
	}

	// No pipeline means there is nothing to settle.
	if (!pipeline_) {
		actualGstState_.store(
			GST_STATE_NULL,
			std::memory_order_release);

		lifecycle_.store(
			PipelineLifecycle::Idle,
			std::memory_order_release);

		return true;
	}

	// Request PAUSED and return immediately. The pipeline STATE_CHANGED bus
	// message is the normal completion mechanism and publishes Idle when the
	// transition has really settled.
	GstStateChangeReturn ret =
		gst_element_set_state(
			pipeline_,
			GST_STATE_PAUSED);

	if (ret == GST_STATE_CHANGE_FAILURE) {
		// PAUSED retention failed. READY is a safe warm-ish fallback; it gives
		// up decoder reuse but still keeps playbin3 itself alive.
		LOG_WARNING(
			"GStreamerVideo",
			"unload(): PAUSED request failed; falling back asynchronously to READY.");

		ret = gst_element_set_state(
			pipeline_,
			GST_STATE_READY);

		if (ret == GST_STATE_CHANGE_FAILURE) {
			lifecycle_.store(
				PipelineLifecycle::Failed,
				std::memory_order_release);

			LOG_ERROR(
				"GStreamerVideo",
				"unload(): READY fallback request also failed.");

			return false;
		}

		if (ret == GST_STATE_CHANGE_SUCCESS ||
			ret == GST_STATE_CHANGE_NO_PREROLL)
		{
			actualGstState_.store(
				GST_STATE_READY,
				std::memory_order_release);

			lifecycle_.store(
				PipelineLifecycle::Idle,
				std::memory_order_release);
		}

		return true;
	}

	// If GStreamer completed synchronously there may be no useful future bus
	// transition to wait for. Publish the confirmed state directly.
	if (ret == GST_STATE_CHANGE_SUCCESS ||
		ret == GST_STATE_CHANGE_NO_PREROLL)
	{
		actualGstState_.store(
			GST_STATE_PAUSED,
			std::memory_order_release);

		lifecycle_.store(
			PipelineLifecycle::Idle,
			std::memory_order_release);
	}

	// GST_STATE_CHANGE_ASYNC intentionally leaves lifecycle_ == Draining.
	// STATE_CHANGED will publish Idle when PAUSED has actually settled.
	return true;
}

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

	double X = (RHS1 * M22 - RHS2 * M12) / denom;
	double Y = (M11 * RHS2 - M21 * RHS1) / denom;

	double g = X - 1.0;
	double h = Y - 1.0;

	double a = X * B.x - A.x;
	double d = X * B.y - A.y;
	double b = Y * D.x - A.x;
	double e = Y * D.y - A.y;
	double c = A.x;
	double f = A.y;

	std::array<double, 9> Hf = { a, b, c,
								 d, e, f,
								 g, h, 1.0 };

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

	for (double& val : H) {
		val /= H[8];
	}

	H[0] *= width; H[1] *= width; H[2] *= width;
	H[3] *= height; H[4] *= height; H[5] *= height;

	return H;
}

bool GStreamerVideo::createPipelineIfNeeded() {
	if (pipeline_) {
		return true;
	}

	pipeline_ = gst_element_factory_make("playbin3", "player");
	videoSink_ = gst_element_factory_make("appsink", "video_sink");

	if (!pipeline_ || !videoSink_) {
		LOG_DEBUG("Video", "Could not create GStreamer elements");
		lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
		return false;
	}

	// instant-uri is a persistent playbin3 mode, not a one-shot trigger.
	// Enable it once for the lifetime of this retained pipeline. Cold opens are
	// unaffected because there is no current URI to replace; warm PAUSED opens
	// can then retarget by changing only the uri property.
	instantUriEnabled_ =
		g_object_class_find_property(
			G_OBJECT_GET_CLASS(pipeline_),
			"instant-uri") != nullptr;

	if (instantUriEnabled_) {
		g_object_set(
			pipeline_,
			"instant-uri", TRUE,
			nullptr);
	}

	audioSink_ = gst_element_factory_make("appsink", "audio_sink");
	if (!audioSink_) {
		LOG_ERROR("GStreamerVideo", "Could not create audio appsink");
		lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
		return false;
	}

	if (!cbCtx_) {
		cbCtx_ = new CallbackCtx;
		g_ref_count_init(&cbCtx_->ref);
		cbCtx_->self.store(weak_from_this());
		cbCtx_->epoch.store(playbackEpoch_.load(std::memory_order_acquire), std::memory_order_release);
	}

	// playbin3 emits this whenever it actually creates a new element. Keeping
	// one handler for both CPU tuning and hardware-decoder diagnostics makes
	// decoder reuse directly visible in the log: a reused decoder does not
	// generate another element-setup callback.
	elementSetupHandlerId_ = g_signal_connect(
		pipeline_,
		"element-setup",
		G_CALLBACK(elementSetupCallback),
		this);

	g_object_set(audioSink_,
		"emit-signals", FALSE,
		"max-buffers", 16,
		"qos", FALSE,
		"drop", TRUE,
		"sync", TRUE,
		"async", FALSE,
		"enable-last-sample", FALSE,
		"wait-on-eos", FALSE,
		nullptr);

	GstAppSinkCallbacks audioCbs = {};
	audioCbs.new_sample = &GStreamerVideo::on_audio_new_sample;

	gst_app_sink_set_callbacks(
		GST_APP_SINK(audioSink_),
		&audioCbs,
		cbCtxRef(cbCtx_),
		&GStreamerVideo::cbCtxUnref
	);

	int rate = AudioBus::instance().dev_rate();
	int channels = AudioBus::instance().dev_channels();
	Uint16 fmt = AudioBus::instance().dev_fmt();

	const char* gstFmt = sdl_to_gst_fmt(fmt);
	if (!gstFmt) {
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
		gstFmt = "S16LE";
#else
		gstFmt = "S16BE";
#endif
	}

	std::ostringstream ss;
	ss << "audio/x-raw"
		<< ",format=" << gstFmt
		<< ",layout=interleaved"
		<< ",rate=" << rate
		<< ",channels=" << channels;

	GstCaps* acaps = gst_caps_from_string(ss.str().c_str());
	gst_app_sink_set_caps(GST_APP_SINK(audioSink_), acaps);
	gst_caps_unref(acaps);

	gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
	flags &= ~(1 << 4);
	g_object_set(pipeline_, "flags", flags, nullptr);

	g_object_set(videoSink_,
		"emit-signals", FALSE,
		"max-buffers", 1,
		"qos", TRUE,
		"drop", TRUE,
		"sync", TRUE,
		"enable-last-sample", FALSE,
		"wait-on-eos", FALSE,
		NULL);

	GstAppSinkCallbacks videoCbs = {};
	videoCbs.new_preroll = &GStreamerVideo::on_new_preroll;
	videoCbs.new_sample = &GStreamerVideo::on_new_sample;

	gst_app_sink_set_callbacks(
		GST_APP_SINK(videoSink_),
		&videoCbs,
		cbCtxRef(cbCtx_),
		&GStreamerVideo::cbCtxUnref
	);

	loggedGpu_ = loggedUpload_ = false;
	gpuInterop_.reset();
	if (Configuration::HardwareVideoAccel && !hasPerspective_ && !disableInterop_) {
#if !defined(RETROFE_HAVE_EGL_DMABUF) && !defined(RETROFE_HAVE_GST_GL)
		NativeVideoInterop::initializeGlobal(SDL::getRenderer(monitor_));
#endif
		gpuInterop_ = std::make_unique<NativeVideoInterop>(SDL::getRenderer(monitor_));
		if (gpuInterop_->available()) {
			gpuInterop_->configure(pipeline_);
#ifdef RETROFE_HAVE_GST_GL
			glPipelineActive_.store(true);
#endif
		}
		else LOG_INFO("GStreamerVideo", std::string("GPU texture interop unavailable: ") + gpuInterop_->reason());
	}
	if (Configuration::HardwareVideoAccel && hasPerspective_)
		LOG_INFO("GStreamerVideo", "GPU texture interop unavailable: perspective filter requires system-memory RGBA");
	GstCaps* videoCaps = nullptr;
	if (hasPerspective_) {
		videoCaps = gst_caps_from_string(
			"video/x-raw,format=(string)RGBA");
		sdlFormat_ = SDL_PIXELFORMAT_ABGR8888;
		LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_ABGR8888 (Perspective enabled)");
	}
	else {
		if (Configuration::HardwareVideoAccel) {
			videoCaps = gst_caps_from_string(
				gpuInterop_ && gpuInterop_->available()
				? NativeVideoInterop::caps()
				: "video/x-raw,format=(string)NV12");
			sdlFormat_ = gpuInterop_ && gpuInterop_->available() ? NativeVideoInterop::pixelFormat() : SDL_PIXELFORMAT_NV12;
			LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_NV12 (HW accel: true)");
		}
		else {
			videoCaps = gst_caps_from_string(
				"video/x-raw,format=(string)I420");
			sdlFormat_ = SDL_PIXELFORMAT_IYUV;
			LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_IYUV (HW accel: false)");
		}
	}

	gst_app_sink_set_caps(GST_APP_SINK(videoSink_), videoCaps);
	gst_caps_unref(videoCaps);

	if (hasPerspective_) {
		perspective_ = gst_element_factory_make("perspective", "perspective");
		if (!perspective_) {
			lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
			return false;
		}

		GstElement* videoBin = gst_bin_new("video_bin");
		if (!videoBin) {
			lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
			return false;
		}

		gst_bin_add_many(GST_BIN(videoBin), perspective_, videoSink_, nullptr);

		if (!gst_element_link(perspective_, videoSink_)) {
			lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
			return false;
		}

		GstPad* perspectiveSinkPad = gst_element_get_static_pad(perspective_, "sink");
		if (!perspectiveSinkPad) {
			lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
			return false;
		}
		GstPad* ghostPad = gst_ghost_pad_new("sink", perspectiveSinkPad);
		gst_object_unref(perspectiveSinkPad);
		if (!gst_element_add_pad(videoBin, ghostPad)) {
			lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
			return false;
		}
		gst_object_ref_sink(videoBin);
		g_object_set(pipeline_, "video-sink", videoBin, nullptr);
		gst_object_unref(videoBin);
	}
	else {
		GstElement* output = gpuInterop_ && gpuInterop_->available() ? gpuInterop_->wrapSink(videoSink_) : videoSink_;
		if (!output) {
			LOG_WARNING("GStreamerVideo", "Could not construct GPU video sink; using CPU texture upload");
			glPipelineActive_.store(false);
			disableInterop_ = true;
			gpuInterop_.reset();
			auto* fallbackCaps = gst_caps_from_string("video/x-raw,format=NV12,pixel-aspect-ratio=1/1");
			gst_app_sink_set_caps(GST_APP_SINK(videoSink_), fallbackCaps);
			gst_caps_unref(fallbackCaps);
			sdlFormat_ = SDL_PIXELFORMAT_NV12;
			output = videoSink_;
		}
		gst_object_ref_sink(output);
		g_object_set(pipeline_, "video-sink", output, nullptr);
		gst_object_unref(output);
	}

	GstPad* sinkPad = gst_element_get_static_pad(videoSink_, "sink");
	if (sinkPad) {
		if (padProbeId_ != 0) {
			gst_pad_remove_probe(sinkPad, padProbeId_);
			padProbeId_ = 0;
		}

		padProbeId_ = gst_pad_add_probe(
			sinkPad,
			GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
			&GStreamerVideo::padProbeCallback,
			cbCtxRef(cbCtx_),
			&GStreamerVideo::cbCtxUnref
		);

		gst_object_unref(sinkPad);
	}
	else {
		lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
		return false;
	}
	gst_object_ref_sink(audioSink_);
	g_object_set(pipeline_, "audio-sink", audioSink_, nullptr);
	gst_object_unref(audioSink_);

	if (GstBus* bus = gst_element_get_bus(pipeline_)) {
		busWatchId_ = GlibLoop::instance().addBusWatch(
			bus,
			+[](GstBus* b, GstMessage* m, gpointer ud) -> gboolean {
				return GStreamerVideo::busCallback(b, m, ud);
			},
			cbCtxRef(cbCtx_),
			&GStreamerVideo::cbCtxUnref
		);
		gst_object_unref(bus);
	}

	return true;
}

VideoSnapshot GStreamerVideo::getSnapshot() const {
	VideoSnapshot snap;

	// Load the hardware truth and the UI intent
	GstState actual = actualGstState_.load(std::memory_order_acquire);
	PlaybackState pState = playbackState_.load(std::memory_order_acquire);

	// Map Target State (What the UI wants)
	snap.targetState = (pState == PlaybackState::Playing) ? VideoState::Playing :
		(pState == PlaybackState::Paused) ? VideoState::Paused : VideoState::None;

	// Map Actual State (What the hardware is doing)
	snap.actualState = (actual == GST_STATE_PLAYING) ? VideoState::Playing :
		(actual == GST_STATE_PAUSED) ? VideoState::Paused : VideoState::None;

	auto currentLife = lifecycle_.load(std::memory_order_acquire);
	snap.pipelineReady = (currentLife == PipelineLifecycle::Ready);
	snap.hasError = hasError();
	snap.hasFinishedLoops = loopsFinished_.load(std::memory_order_acquire);
	snap.hasVideoStream = hasVideoStream_.load(std::memory_order_acquire);

	return snap;
}

bool GStreamerVideo::open(const std::string& file) {
	return openMedia(file, false);
}

bool GStreamerVideo::openMedia(const std::string& file, bool cpuFallback) {
	if (!initialized_)
		return false;

	if (lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Draining) {
		LOG_DEBUG(
			"GStreamerVideo",
			"open(): rejected while pipeline is draining: " + file);
		return false;
	}

	// Only an internal recovery open preserves CPU fallback. A new media
	// request retries GPU negotiation by rebuilding a retained fallback graph.
	if (!cpuFallback && disableInterop_) {
		stop();
		disableInterop_ = false;
	}

	const bool hadPipeline = pipeline_ != nullptr;

	// lifecycle_ == Idle is published only after the retained pipeline has
	// settled, so actualGstState_ is sufficient here. Avoid even a zero-time
	// gst_element_get_state() query on the caller/render thread.
	const GstState stateBeforeOpen =
		actualGstState_.load(std::memory_order_acquire);

	const uint64_t newEpoch = nextUniquePlaybackEpoch_++;
	playbackEpoch_.store(newEpoch, std::memory_order_release);

	if (cbCtx_) {
		cbCtx_->epoch.store(newEpoch, std::memory_order_release);
	}

	currentFile_ = file;
	startupOpenedNs_ = SDL_GetTicksNS();
	startupWorkerNs_.store(0);
	startupSampleNs_.store(0);
	startupLogged_ = false;
	startupInstantSwitch_ = false;

	loggedGpu_ = false;
	loggedUpload_ = false;
	isTextureReady_ = false;

	dimensions_.store({ -1, -1 }, std::memory_order_release);
	loopsFinished_.store(false, std::memory_order_release);

	if (!createPipelineIfNeeded())
		return false;

	// If this is a reused pooled instance that unload() successfully kept in
	// PAUSED, playbin3 can switch the URI without dropping decodebin3 to READY.
	// GStreamer can then reuse compatible decoders such as d3d11h264dec.
	const bool canInstantSwitch =
		hadPipeline &&
		instantUriEnabled_ &&
		stateBeforeOpen == GST_STATE_PAUSED;

	startupInstantSwitch_ = canInstantSwitch;

	awaitingInitialPreroll_.store(true, std::memory_order_release);
	lifecycle_.store(PipelineLifecycle::Starting, std::memory_order_release);
	playbackState_.store(PlaybackState::Paused, std::memory_order_release);

	gchar* uri = gst_filename_to_uri(file.c_str(), nullptr);
	if (!uri) {
		awaitingInitialPreroll_.store(false, std::memory_order_release);
		lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
		return false;
	}

	if (canInstantSwitch) {
		// instant-uri was enabled when this playbin3 was created. For a retained
		// PAUSED pipeline, changing uri is the complete instant-retarget operation.
		startupWorkerNs_.store(SDL_GetTicksNS(), std::memory_order_release);

		g_object_set(
			pipeline_,
			"uri", uri,
			nullptr);

		LOG_DEBUG(
			"GStreamerVideo",
			"playbin3 instant URI switch: " + file);
	}
	else {
		g_object_set(pipeline_, "uri", uri, nullptr);
	}

	g_free(uri);

	if (!canInstantSwitch) {
		std::weak_ptr<GStreamerVideo> weak = weak_from_this();
		GstElement* p = pipeline_;
		gst_object_ref(p);

		ThreadPool::getInstance().enqueue(
			[weak, p, file, openEpoch = newEpoch]() {
				if (auto self = weak.lock(); self && self->isCurrentEpoch(openEpoch)) {
					self->startupWorkerNs_.store(
						SDL_GetTicksNS(),
						std::memory_order_release);
				}

				GstStateChangeReturn ret =
					gst_element_set_state(p, GST_STATE_PAUSED);

				gst_object_unref(p);

				if (ret != GST_STATE_CHANGE_FAILURE)
					return;

				LOG_ERROR(
					"GStreamerVideo",
					"Async pause failed for " + file);

				if (auto self = weak.lock()) {
					if (self->playbackEpoch_.load(std::memory_order_acquire) != openEpoch)
						return;

					if (self->lifecycle_.load(std::memory_order_acquire) ==
						PipelineLifecycle::Draining)
					{
						return;
					}

					const bool retryGL = self->glPipelineActive_.exchange(false);

					self->lifecycle_.store(
						retryGL ? PipelineLifecycle::Starting : PipelineLifecycle::Failed,
						std::memory_order_release);
					self->awaitingInitialPreroll_.store(false, std::memory_order_release);

					if (retryGL) {
						self->pendingCpuFallback_.store(true, std::memory_order_release);
					}
				}
			});
	}

	if (videoSourceId_ == 0) {
		videoSourceId_ = AudioBus::instance().addSource("video-preview");
		audioHandle_ = AudioBus::instance().getHandle(videoSourceId_);
	}

	AudioBus::instance().setGain(audioHandle_, 0.0f);
	return true;
}

GstPadProbeReturn GStreamerVideo::padProbeCallback(
	GstPad* /*pad*/,
	GstPadProbeInfo* info,
	gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx)
		return GST_PAD_PROBE_OK;

	// Pin the GStreamerVideo instance for the duration of this callback.
	auto video = ctx->self.load().lock();
	if (!video)
		return GST_PAD_PROBE_OK;

	const uint64_t epoch = ctx->epoch.load(std::memory_order_acquire);

	// Ignore events belonging to a stale playback generation.
	if (epoch != video->playbackEpoch_.load(std::memory_order_acquire))
		return GST_PAD_PROBE_OK;

	GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
	if (!ev)
		return GST_PAD_PROBE_OK;

	if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
		GstCaps* caps = nullptr;
		gst_event_parse_caps(ev, &caps);

		if (!caps)
			return GST_PAD_PROBE_OK;

		const GstStructure* s = gst_caps_get_structure(caps, 0);

		int w = 0;
		int h = 0;

		if (gst_structure_get_int(s, "width", &w) &&
			gst_structure_get_int(s, "height", &h) &&
			w > 0 && h > 0)
		{
			// Playback may have changed while processing the event.
			if (epoch != video->playbackEpoch_.load(std::memory_order_acquire))
				return GST_PAD_PROBE_OK;

			video->dimensions_.store({ w, h }, std::memory_order_release);

			if (video->hasPerspective_) {
				if (w != video->lastPerspectiveW_ ||
					h != video->lastPerspectiveH_)
				{
					video->lastPerspectiveW_ = w;
					video->lastPerspectiveH_ = h;

					std::array<Point2D, 4> box = {
						Point2D(
							double(video->perspectiveCorners_[0]),
							double(video->perspectiveCorners_[1])),
						Point2D(
							double(video->perspectiveCorners_[2]),
							double(video->perspectiveCorners_[3])),
						Point2D(
							double(video->perspectiveCorners_[4]),
							double(video->perspectiveCorners_[5])),
						Point2D(
							double(video->perspectiveCorners_[6]),
							double(video->perspectiveCorners_[7]))
					};

					const auto mat =
						computePerspectiveMatrixFromCorners(w, h, box);

					struct Task {
						std::weak_ptr<GStreamerVideo> self;
						std::array<double, 9> matrix;
						uint64_t epoch;
					};

					auto* task = new Task{
						video->weak_from_this(),
						mat,
						epoch
					};

					g_main_context_invoke_full(
						GlibLoop::instance().context(),
						G_PRIORITY_DEFAULT,
						[](gpointer data) -> gboolean {
							auto* t = static_cast<Task*>(data);
							if (!t)
								return G_SOURCE_REMOVE;

							auto v = t->self.lock();
							if (!v || !v->perspective_)
								return G_SOURCE_REMOVE;

							if (t->epoch !=
								v->playbackEpoch_.load(
									std::memory_order_acquire))
							{
								return G_SOURCE_REMOVE;
							}

							if (v->perspective_gva_) {
								g_value_array_free(v->perspective_gva_);
								v->perspective_gva_ = nullptr;
							}

							v->perspective_gva_ =
								g_value_array_new(9);

							GValue val = G_VALUE_INIT;
							g_value_init(&val, G_TYPE_DOUBLE);

							for (double e : t->matrix) {
								g_value_set_double(&val, e);
								g_value_array_append(
									v->perspective_gva_,
									&val);
							}

							g_value_unset(&val);

							g_object_set(
								G_OBJECT(v->perspective_),
								"matrix",
								v->perspective_gva_,
								nullptr);

							return G_SOURCE_REMOVE;
						},
						task,
						[](gpointer data) {
							delete static_cast<Task*>(data);
						}
					);
				}
			}
		}
	}

	return GST_PAD_PROBE_OK;
}

void GStreamerVideo::elementSetupCallback([[maybe_unused]] GstElement* playbin,
	GstElement* element,
	[[maybe_unused]] gpointer data) {
	const gchar* name = GST_OBJECT_NAME(element);

	auto has_prop = [](GstElement* e, const char* p) {
		return g_object_class_find_property(
			G_OBJECT_GET_CLASS(e), p) != nullptr;
		};

	if (g_str_has_prefix(name, "queue") ||
		g_str_has_prefix(name, "aqueue") ||
		g_str_has_prefix(name, "vqueue"))
	{
		// Increase buffer count to prevent "Queue full" warnings with audio-only files
		if (has_prop(element, "max-size-buffers"))
			g_object_set(element, "max-size-buffers", 100, NULL);

		if (has_prop(element, "max-size-bytes"))
			g_object_set(element, "max-size-bytes", (guint64)(15 * 1024 * 1024), NULL);

		if (has_prop(element, "max-size-time"))
			g_object_set(element, "max-size-time", (guint64)(500 * GST_MSECOND), NULL);

		if (has_prop(element, "flush-on-eos"))
			g_object_set(element, "flush-on-eos", TRUE, NULL);

		if (has_prop(element, "silent"))
			g_object_set(element, "silent", TRUE, NULL);
	}


	if (Configuration::HardwareVideoAccel && GST_IS_VIDEO_DECODER(element)) {
		if (auto* factory = gst_element_get_factory(element)) {
			LOG_INFO(
				"GStreamerVideo",
				std::string("playbin3 created video decoder: ") +
				GST_OBJECT_NAME(factory) +
				" instance=" + std::to_string(reinterpret_cast<uintptr_t>(element)));
		}
	}

	if (!Configuration::HardwareVideoAccel &&
		GST_IS_VIDEO_DECODER(element))
	{
		if (has_prop(element, "thread-type") && has_prop(element, "max-threads")) {
			g_object_set(element,
				"thread-type", Configuration::AvdecThreadType,
				"max-threads", Configuration::AvdecMaxThreads,
				"direct-rendering", FALSE,
				"std-compliance", 0,
				nullptr);
		}
	}
}


void GStreamerVideo::completeInitialPreroll(uint64_t epoch) {
	if (epoch != playbackEpoch_.load(std::memory_order_acquire))
		return;

	if (lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Starting)
		return;

	if (!awaitingInitialPreroll_.exchange(false, std::memory_order_acq_rel))
		return;

	// A current-epoch video sample is stronger evidence of readiness than a
	// pipeline-wide ASYNC_DONE, especially during playbin3 instant-uri changes
	// where the pipeline itself may remain PAUSED for the entire switch.
	hasVideoStream_.store(true, std::memory_order_release);
	lifecycle_.store(PipelineLifecycle::Ready, std::memory_order_release);
}

GstFlowReturn GStreamerVideo::on_new_preroll(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx) return GST_FLOW_OK;

	auto video = ctx->self.load().lock();
	if (!video) return GST_FLOW_OK;

	const uint64_t callbackEpoch = ctx->epoch.load(std::memory_order_acquire);

	// STRICT EPOCH VALIDATION: Drop stale frames instantly.
	if (callbackEpoch != video->playbackEpoch_.load(std::memory_order_acquire))
		return GST_FLOW_OK;

	GstSample* s = gst_app_sink_pull_preroll(sink);
	if (!s) return GST_FLOW_OK;
	video->hasVideoStream_.store(true, std::memory_order_release);
	Uint64 unset = 0;
	video->startupSampleNs_.compare_exchange_strong(unset, SDL_GetTicksNS());

	VideoDim currentDim = video->dimensions_.load(std::memory_order_acquire);
	if (currentDim.w <= 0 || currentDim.h <= 0) {
		if (GstCaps* caps = gst_sample_get_caps(s)) {
			GstVideoInfo info;
			if (gst_video_info_from_caps(&info, caps)) {
				video->dimensions_.store(
					{ GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info) },
					std::memory_order_release
				);
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(video->sampleMutex_);
		if (video->stagedSample_.sample) {
			gst_sample_unref(video->stagedSample_.sample);
		}
		video->stagedSample_.sample = s;
		video->stagedSample_.epoch = callbackEpoch;
	}

	video->completeInitialPreroll(callbackEpoch);
	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx) return GST_FLOW_OK;

	auto video = ctx->self.load().lock();
	if (!video) return GST_FLOW_OK;

	const uint64_t callbackEpoch = ctx->epoch.load(std::memory_order_acquire);

	if (callbackEpoch != video->playbackEpoch_.load(std::memory_order_acquire))
		return GST_FLOW_OK;

	GstSample* s = gst_app_sink_pull_sample(sink);
	if (!s) return GST_FLOW_OK;
	video->hasVideoStream_.store(true, std::memory_order_release);
	Uint64 unset = 0;
	video->startupSampleNs_.compare_exchange_strong(unset, SDL_GetTicksNS());

	VideoDim currentDim = video->dimensions_.load(std::memory_order_acquire);
	if (currentDim.w <= 0 || currentDim.h <= 0) {
		if (GstCaps* caps = gst_sample_get_caps(s)) {
			GstVideoInfo info;
			if (gst_video_info_from_caps(&info, caps)) {
				video->dimensions_.store(
					{ GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info) },
					std::memory_order_release
				);
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(video->sampleMutex_);
		if (video->stagedSample_.sample) {
			gst_sample_unref(video->stagedSample_.sample);
		}
		video->stagedSample_.sample = s;
		video->stagedSample_.epoch = callbackEpoch;
	}

	video->completeInitialPreroll(callbackEpoch);
	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_audio_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx) return GST_FLOW_OK;

	auto video = ctx->self.load().lock();
	if (!video) return GST_FLOW_OK;

	const uint64_t callbackEpoch = ctx->epoch.load(std::memory_order_acquire);
	if (callbackEpoch != video->playbackEpoch_.load(std::memory_order_acquire))
		return GST_FLOW_OK;

	GstSample* s = gst_app_sink_pull_sample(sink);
	if (!s) return GST_FLOW_OK;

	const bool active = video->audioHandle_ &&
		video->lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Ready;

	if (!active) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	GstBuffer* b = gst_sample_get_buffer(s);
	if (!b || GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_CORRUPTED)) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	GstMapInfo mi{};
	if (gst_buffer_map(b, &mi, GST_MAP_READ)) {

		// --- TRUE SEMANTIC DISCONTINUITY ---
		// If this is the first audio frame of this new epoch, trigger the fade-in!
		uint64_t currentFade = video->lastFadedEpoch_.load(std::memory_order_acquire);
		if (currentFade != callbackEpoch) {
			if (video->lastFadedEpoch_.compare_exchange_strong(currentFade, callbackEpoch)) {
				AudioBus::instance().triggerFadeIn(video->audioHandle_);
			}
		}

		AudioBus::instance().push(video->audioHandle_, mi.data, (int)mi.size);
		gst_buffer_unmap(b, &mi);
	}

	gst_sample_unref(s);
	return GST_FLOW_OK;
}

void GStreamerVideo::createSdlTexture() {
	const PipelineLifecycle life =
		lifecycle_.load(std::memory_order_acquire);

	if (!pipeline_ ||
		(life != PipelineLifecycle::Starting &&
			life != PipelineLifecycle::Ready))
	{
		return;
	}

	VideoDim current = dimensions_.load(std::memory_order_acquire);
	int w = current.w;
	int h = current.h;

	bool needsRecreate = (allocatedWidth_ != w ||
		allocatedHeight_ != h ||
		allocatedFormat_ != sdlFormat_ ||
		texture_ == nullptr);

	if (!needsRecreate) return;

	if (w <= 0 || h <= 0) {
		destroyTextures();
		return;
	}

	destroyTextures();

	texture_ = SDL_CreateTexture(
		SDL::getRenderer(monitor_), sdlFormat_, SDL_TEXTUREACCESS_STREAMING, w, h);

	if (!texture_) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_CreateTexture failed: ") + SDL_GetError());
		destroyTextures();
		return;
	}

	SDL_SetTextureBlendMode(texture_, softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);

	allocatedWidth_ = w;
	allocatedHeight_ = h;
	allocatedFormat_ = sdlFormat_;
	isTextureReady_ = false;
}

void GStreamerVideo::updateFrame() {
	if (pendingCpuFallback_.exchange(false, std::memory_order_acq_rel)) {
		const auto file = currentFile_;
		disableInterop_ = true;
		stop();
		if (!file.empty() && !openMedia(file, true)) lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
		return;
	}
	GstSample* sampleToProcess = nullptr;

	{
		std::lock_guard<std::mutex> lock(sampleMutex_);
		if (stagedSample_.sample && stagedSample_.epoch == playbackEpoch_.load(std::memory_order_acquire)) {
			sampleToProcess = stagedSample_.sample;
			stagedSample_.sample = nullptr; // Consume!
			// No state mutation here whatsoever. The rendering system only consumes frames.
		}
	}

	if (!sampleToProcess) return;

	GstBuffer* buf = gst_sample_get_buffer(sampleToProcess);
	const GstCaps* caps = gst_sample_get_caps(sampleToProcess);
	if (!buf || !caps) { gst_sample_unref(sampleToProcess); return; }

	GstVideoInfo info;
	if (!gst_video_info_from_caps(&info, caps)) { gst_sample_unref(sampleToProcess); return; }

	const int frameW = GST_VIDEO_INFO_WIDTH(&info);
	const int frameH = GST_VIDEO_INFO_HEIGHT(&info);
	if (frameW <= 0 || frameH <= 0) { gst_sample_unref(sampleToProcess); return; }

	VideoDim currentDim = dimensions_.load(std::memory_order_acquire);
	if (currentDim.w <= 0 || currentDim.h <= 0) {
		dimensions_.store({ frameW, frameH }, std::memory_order_release);
	}

	dimensions_.store({ frameW, frameH }, std::memory_order_release);
	if (gpuInterop_ && gpuInterop_->available()) {
		if (SDL_Texture* imported = gpuInterop_->copy(sampleToProcess)) {
			if (texture_ && texture_ != gpuTexture_) SDL_DestroyTexture(texture_);
			texture_ = gpuTexture_ = imported;
#ifdef RETROFE_HAVE_EGL_DMABUF
			dimensions_.store({gpuInterop_->width(), gpuInterop_->height()}, std::memory_order_release);
#endif
			++gpuFrameCount_;
			SDL_SetTextureBlendMode(texture_, softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND);
			isTextureReady_ = true;
			logStartupTiming();
			if (!loggedGpu_) {
				LOG_INFO("GStreamerVideo", std::string("GPU texture interop ACTIVE: ") + gpuInterop_->description() + "; monitor " + std::to_string(monitor_) + "; " + currentFile_);
				loggedGpu_ = true;
			}
			gst_sample_unref(sampleToProcess);
			return;
		}
		if (!loggedUpload_) LOG_INFO("GStreamerVideo", std::string("GPU texture interop fallback: ") + gpuInterop_->reason() + "; " + currentFile_);
#ifdef RETROFE_HAVE_EGL_DMABUF
		// Never CPU-map a tiled DMA_DRM frame as ordinary NV12/RGBA. Reopen
		// with system-memory negotiation through the existing recovery path.
		isTextureReady_ = false;
		pendingCpuFallback_.store(true, std::memory_order_release);
		gst_sample_unref(sampleToProcess);
		return;
#endif
	}
	if (texture_ == gpuTexture_) { texture_ = nullptr; gpuTexture_ = nullptr; }
	createSdlTexture();

	GstVideoFrame frame;
	if (!gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
		gst_sample_unref(sampleToProcess);
		return;
	}

	bool ok = false;
	if (!loggedUpload_) {
		LOG_INFO("GStreamerVideo", "Video uses CPU texture upload; monitor " + std::to_string(monitor_) + "; " + currentFile_);
		loggedUpload_ = true;
	}
	if (texture_) {
		switch (sdlFormat_) {
			case SDL_PIXELFORMAT_IYUV:     ok = updateTextureFromFrameIYUV(texture_, &frame); break;
			case SDL_PIXELFORMAT_NV12:     ok = updateTextureFromFrameNV12(texture_, &frame); break;
			case SDL_PIXELFORMAT_ABGR8888: ok = updateTextureFromFrameRGBA(texture_, &frame); break;
			default: break;
		}
	}

	gst_video_frame_unmap(&frame);
	gst_sample_unref(sampleToProcess);

	if (ok) {
		isTextureReady_ = true;
		logStartupTiming();
	}
}

void GStreamerVideo::logStartupTiming() {
	if (startupLogged_) return;
	startupLogged_ = true;
	const auto worker = startupWorkerNs_.load();
	const auto sample = startupSampleNs_.load();
	const auto ready = SDL_GetTicksNS();
	if (!startupOpenedNs_ || worker < startupOpenedNs_ || sample < worker) return;
	auto ms = [](Uint64 ns) { return std::to_string(double(ns) / 1000000.0); };
	LOG_DEBUG("GStreamerVideo", "Startup timing ms: setup_queue=" + ms(worker - startupOpenedNs_) +
		" pipeline_to_sample=" + ms(sample - worker) +
		" sample_to_texture=" + ms(ready - sample) +
		" total=" + ms(ready - startupOpenedNs_) +
		" path=" + (gpuTexture_ ? std::string("GPU") : std::string("CPU")) +
		" switch=" + (startupInstantSwitch_ ? std::string("instant") : std::string("cold")) +
		"; " + currentFile_);
}

bool GStreamerVideo::updateTextureFromFrameIYUV(SDL_Texture* texture, GstVideoFrame* frame) const {
	const auto* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const auto* srcU = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));
	const auto* srcV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 2));

	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);
	const int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 2);

	if (!SDL_UpdateYUVTexture(texture, nullptr, srcY, strideY, srcU, strideU, srcV, strideV)) {
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameNV12(SDL_Texture* texture, GstVideoFrame* frame) const {
	const auto* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const auto* srcUV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));

	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideUV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);

	if (!SDL_UpdateNVTexture(texture, nullptr, srcY, strideY, srcUV, strideUV)) {
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameRGBA(SDL_Texture* texture, GstVideoFrame* frame) const {

	const void* src_pixels = GST_VIDEO_FRAME_PLANE_DATA(frame, 0);
	const int src_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);

	if (!SDL_UpdateTexture(texture, nullptr, src_pixels, src_pitch)) {
		return false;
	}

	return true;
}

VideoDim GStreamerVideo::getDimensions() {
	return dimensions_.load(std::memory_order_acquire);
}

bool GStreamerVideo::isPlaying() {
	return playbackState_.load(std::memory_order_acquire) == PlaybackState::Playing;
}

void GStreamerVideo::setVolume(float volume) {
	if (!audioHandle_) return;
	volume_ = volume;
	float finalGain = std::clamp(volume_, 0.0f, 1.0f);
	if (Configuration::MuteVideo || finalGain < 0.01f) finalGain = 0.0f;
	AudioBus::instance().setGain(audioHandle_, finalGain);
}

void GStreamerVideo::skipForward() {
	if (!isPipelineReady() || !pipeline_) return;

	gint64 currentPos = 0;
	gint64 duration = 0;

	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos) ||
		!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
		return;
	}

	gint64 newPos = currentPos + (60 * GST_SECOND);
	if (newPos > duration) {
		newPos = duration - (GST_SECOND / 10);  // 100ms from end
	}

	gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
}

void GStreamerVideo::skipBackward() {
	if (!isPipelineReady() || !pipeline_) return;

	gint64 currentPos = 0;
	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos)) return;

	gint64 newPos = (currentPos > 60 * GST_SECOND) ? (currentPos - 60 * GST_SECOND) : 0;

	gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
}

void GStreamerVideo::skipForwardp() {
	if (!isPipelineReady() || !pipeline_) return;

	gint64 currentPos = 0;
	gint64 duration = 0;

	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos) ||
		!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
		return;
	}

	gint64 skipAmount = duration / 20;  // 5% of duration
	gint64 newPos = currentPos + skipAmount;
	if (newPos > duration) {
		newPos = duration - (GST_SECOND / 10);
	}

	gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
}

void GStreamerVideo::skipBackwardp() {
	if (!isPipelineReady() || !pipeline_) return;

	gint64 currentPos = 0;
	gint64 duration = 0;

	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos) ||
		!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
		return;
	}

	gint64 skipAmount = duration / 20;  // 5% of duration
	gint64 newPos = (currentPos > skipAmount) ? (currentPos - skipAmount) : 0;

	gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
}

void GStreamerVideo::pause() {
	if (!pipeline_)
		return;

	if (lifecycle_.load(std::memory_order_acquire) !=
		PipelineLifecycle::Ready)
	{
		return;
	}

	if (playbackState_.load(std::memory_order_acquire) ==
		PlaybackState::Paused)
	{
		return;
	}

	playbackState_.store(
		PlaybackState::Paused,
		std::memory_order_release);

	GstElement* p = pipeline_;
	gst_object_ref(p);

	ThreadPool::getInstance().enqueue([p]() {
		gst_element_set_state(
			p,
			GST_STATE_PAUSED);

		gst_object_unref(p);
		});
}

void GStreamerVideo::resume() {
	if (!pipeline_)
		return;

	if (lifecycle_.load(std::memory_order_acquire) !=
		PipelineLifecycle::Ready)
	{
		return;
	}

	if (playbackState_.load(std::memory_order_acquire) ==
		PlaybackState::Playing)
	{
		return;
	}

	playbackState_.store(
		PlaybackState::Playing,
		std::memory_order_release);

	GstElement* p = pipeline_;
	gst_object_ref(p);

	ThreadPool::getInstance().enqueue([p]() {
		gst_element_set_state(
			p,
			GST_STATE_PLAYING);

		gst_object_unref(p);
		});
}

void GStreamerVideo::restart() {
	if (!isPipelineReady() || !pipeline_) return;

	gint64 currentPos = 0;
	if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos)) {
		if (currentPos < (GST_SECOND / 10)) return;
	}

	GstElement* p = pipeline_;
	gst_object_ref(p);

	ThreadPool::getInstance().enqueue([p]() {
		gst_element_seek(p, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, 0,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
		gst_object_unref(p);
		});
}

void GStreamerVideo::loop() {
	if (!isPipelineReady() || !pipeline_) return;

	GstElement* p = pipeline_;
	gst_object_ref(p);

	ThreadPool::getInstance().enqueue([p]() {
		gst_element_seek(p, 1.0, GST_FORMAT_TIME,
			GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
		gst_object_unref(p);
		});
}

unsigned long long GStreamerVideo::getCurrent() {
	if (!pipeline_ || !isPipelineReady()) return 0;

	gint64 ret = 0;
	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &ret))
		ret = 0;
	return static_cast<unsigned long long>(ret);
}

unsigned long long GStreamerVideo::getDuration() {
	if (!pipeline_ || !isPipelineReady()) return 0;
	gint64 ret = 0;
	if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &ret))
		ret = 0;
	return static_cast<unsigned long long>(ret);
}

bool GStreamerVideo::isPaused() {
	return getTargetState() == IVideo::VideoState::Paused;
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
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 1);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::disablePlugin(const std::string& pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory)
	{
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
		hasPerspective_ = true;
	}
	else {
		std::fill(perspectiveCorners_, perspectiveCorners_ + 8, 0);
		hasPerspective_ = false;
	}
}

bool GStreamerVideo::hasFinishedLoops() const {
	return loopsFinished_.load(std::memory_order_acquire);
}
