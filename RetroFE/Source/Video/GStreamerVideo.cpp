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
#include "../Utility/ThreadPool.h"
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
#include <functional>

bool GStreamerVideo::initialized_ = false;
bool GStreamerVideo::pluginsInitialized_ = false;

// Initialize the static Epoch ID generator
std::atomic<uint64_t> GStreamerVideo::nextUniquePlaybackEpoch_{ 1 };
std::mutex GStreamerVideo::transitionSchedulerMutex_;
std::unordered_map<int, GStreamerVideo::TransitionSchedulerState>
    GStreamerVideo::transitionSchedulers_{};

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
static const char* sdl_to_gst_fmt(
	SDL_AudioFormat fmt) {
	switch (fmt) {
		case SDL_AUDIO_U8:
		return "U8";

		case SDL_AUDIO_S8:
		return "S8";

		case SDL_AUDIO_S16LE:
		return "S16LE";

		case SDL_AUDIO_S16BE:
		return "S16BE";

		case SDL_AUDIO_S32LE:
		return "S32LE";

		case SDL_AUDIO_S32BE:
		return "S32BE";

		case SDL_AUDIO_F32LE:
		return "F32LE";

		case SDL_AUDIO_F32BE:
		return "F32BE";

		default:
		return nullptr;
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
	// Factory-owned instances reach destruction after dispose() has completed
	// their worker-side NULL transition and main-thread GPU cleanup.
	if (pipeline_) {
		LOG_WARNING("GStreamerVideo", "Synchronous fallback cleanup of an undisposed pipeline");
		gst_element_set_state(pipeline_, GST_STATE_NULL);
		gst_object_unref(pipeline_);
		pipeline_ = nullptr;
	}
	destroyTextures();
	gpuInterop_.reset();

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

					video->releaseTransitionPermit(
						"retained pipeline settled");
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

				// A pipeline-wide ASYNC_DONE can complete a cold/audio-only
				// startup without a video sample callback. Do not strand a
				// scheduler permit in that case.
				video->releaseTransitionPermit(
					"initial ASYNC_DONE");
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

					std::weak_ptr<GStreamerVideo> weak = video;
					video->enqueueControl([weak, epoch] {
						if (auto self = weak.lock();
							self && self->playbackEpoch_.load(std::memory_order_acquire) == epoch && self->pipeline_)
							gst_element_set_state(self->pipeline_, GST_STATE_PAUSED);
					});

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

				std::weak_ptr<GStreamerVideo> weak = video;
				video->enqueueControl([weak, epoch] {
					if (auto self = weak.lock();
						self && self->playbackEpoch_.load(std::memory_order_acquire) == epoch && self->pipeline_)
						gst_element_set_state(self->pipeline_, GST_STATE_PAUSED);
				});

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

				// Preserve the existing draining/error policy, but never let a
				// failed drain permanently consume the global transition budget.
				video->releaseTransitionPermit(
					"pipeline error while draining");

				break;
			}

			const bool retryGL =
				video->glPipelineActive_.exchange(false);

			video->playbackState_.store(
				PlaybackState::None,
				std::memory_order_release);

			// READY can synchronously tear down decoder work. Keep that off the
			// bus loop and release the transition slot after the request returns.
			std::weak_ptr<GStreamerVideo> weak = video;
			video->enqueueControl([weak, epoch] {
				if (auto self = weak.lock();
					self && self->playbackEpoch_.load(std::memory_order_acquire) == epoch && self->pipeline_) {
					gst_element_set_state(self->pipeline_, GST_STATE_READY);
					self->releaseTransitionPermit("pipeline error");
				}
			});

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
					"GPU pipeline failed; retrying this instance "
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

		// Keep NVIDIA-specific decoders out of playbin autoplugging.
		disablePlugin("nvh264dec");
		disablePlugin("nvh265dec");

		if (Configuration::HardwareVideoAccel)
		{
			const std::string rendererBackend =
				SDL::getRendererBackend(0);

			if (rendererBackend == "direct3d11")
			{
				// Enable both D3D12 and D3D11 decoders.
				// GStreamer's D3D12 decoders (rank primary + 2) offer superior stability
				// and driver correctness across Intel/AMD/NVIDIA and can output D3D11Memory
				// directly to our D3D11 interop, while D3D11 decoders (rank primary + 1)
				// serve as fallback.
				for (const char* codec :
					{ "h264", "h265", "vp9", "mpeg2", "av1" })
				{
					enablePlugin(
						std::string("d3d12") + codec + "dec");
					enablePlugin(
						std::string("d3d11") + codec + "dec");
				}

				enablePlugin("d3d11vp8dec");

				// Do not let Intel QSV win autoplugging when our renderer and
				// zero-copy interop path are explicitly D3D11.
				disablePlugin("qsvh264dec");
				disablePlugin("qsvh265dec");

				LOG_INFO(
					"GStreamerVideo",
					"D3D11 hardware decoding requested (preferring D3D12 decoders with D3D11Memory output); awaiting frame verification");
			}
			else if (rendererBackend == "direct3d12")
			{
				// Match native decoder memory to the actual SDL renderer.
				for (const char* codec :
					{ "h264", "h265", "vp9", "mpeg2", "av1" })
				{
					enablePlugin(
						std::string("d3d12") + codec + "dec");

					disablePlugin(
						std::string("d3d11") + codec + "dec");
				}

				disablePlugin("d3d11vp8dec");

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
	if (!isTextureReady_ ||
		!textureValid_.load(std::memory_order_acquire))
	{
		return nullptr;
	}

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
    ThreadPool& videoControlPool() {
        static ThreadPool pool(3);
        return pool;
    }
    std::atomic<size_t> pendingVideoStops{0};

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

void GStreamerVideo::dispose(GStreamerVideo* video) {
	if (!video) return;
	if (!video->pipeline_ &&
		video->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Stopping) {
		delete video;
		return;
	}

	// The original shared_ptr has reached zero. Keep the raw instance alive
	// through ordered control teardown, then destroy SDL resources on main.
	auto owner = std::shared_ptr<GStreamerVideo>(video, [](GStreamerVideo* done) {
		if (SDL_IsMainThread()) delete done;
		else SDL_RunOnMainThread([](void* data) {
			delete static_cast<GStreamerVideo*>(data);
		}, done, false);
	});
	video->disposing_ = true;
	video->pendingOpenAfterStop_.clear();
	video->beginStop(false);
}

void GStreamerVideo::enqueueControl(std::function<void()> task) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        controlTasks_.push_back(std::move(task));
        if (!controlQueueRunning_) {
            controlQueueRunning_ = true;
            start = true;
        }
    }
    if (start) {
        auto self = shared_from_this();
        (void)videoControlPool().enqueue([self] { self->drainControlQueue(); });
    }
}

void GStreamerVideo::drainControlQueue() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            if (controlTasks_.empty()) {
                controlQueueRunning_ = false;
                return;
            }
            task = std::move(controlTasks_.front());
            controlTasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("GStreamerVideo", std::string("Control task failed: ") + e.what());
        } catch (...) {
            LOG_ERROR("GStreamerVideo", "Control task failed with unknown exception");
        }
    }
}

bool GStreamerVideo::publishLifecycleUnlessStopping(PipelineLifecycle state) {
    auto current = lifecycle_.load(std::memory_order_acquire);
    while (current != PipelineLifecycle::Stopping) {
        if (lifecycle_.compare_exchange_weak(current, state,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }
    return false;
}

void GStreamerVideo::waitForControlTasks() {
    for (;;) {
        videoControlPool().wait();
        SDL_PumpEvents();
        if (pendingVideoStops.load(std::memory_order_acquire) == 0) {
            videoControlPool().wait();
            return;
        }
        SDL_Delay(1);
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
	textureValid_.store(false, std::memory_order_release);
	allocatedWidth_ = 0;
	allocatedHeight_ = 0;
	allocatedFormat_ = SDL_PIXELFORMAT_UNKNOWN;
}

bool GStreamerVideo::stop() {
	return beginStop(false);
}

bool GStreamerVideo::beginStop(bool preservePendingOpen) {
	if (!preservePendingOpen) pendingOpenAfterStop_.clear();
	if (lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Stopping)
		return true;

	if (gpuInterop_) {
		gpuInterop_->discardFrames();
	}

	glPipelineActive_.store(false, std::memory_order_release);
	pendingCpuFallback_.store(false, std::memory_order_release);
	cancelStartupPermitWait();
	resetRetargetState();

	// Invalidate every queued control command / callback belonging to the
	// previous playback generation before touching the pipeline.
	playbackEpoch_.fetch_add(1, std::memory_order_acq_rel);

	if (cbCtx_) {
		cbCtx_->epoch.store(
			0,
			std::memory_order_release);
	}

	// Publish application-visible state immediately.
	lifecycle_.store(
		PipelineLifecycle::Stopping,
		std::memory_order_release);

	playbackState_.store(
		PlaybackState::None,
		std::memory_order_release);

	awaitingInitialPreroll_.store(
		false,
		std::memory_order_release);

	pendingVideoStops.fetch_add(1, std::memory_order_acq_rel);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();
	const Uint64 queuedNs = SDL_GetTicksNS();
	enqueueControl([weak, queuedNs] {
		if (auto self = weak.lock()) {
			const Uint64 queueMs = (SDL_GetTicksNS() - queuedNs) / 1000000;
			if (queueMs > 500)
				LOG_WARNING("GStreamerVideo", "NULL teardown waited " + std::to_string(queueMs) + " ms in control queue");
			self->stopOnControl();
		}
	});
	return true;
}

void GStreamerVideo::stopOnControl() {
	// Earlier commands for this instance have drained from its ordered queue.
	GstElement* pipeline = pipeline_;

	if (pipeline) {
		if (elementSetupHandlerId_ > 0) {
			g_signal_handler_disconnect(
				pipeline,
				elementSetupHandlerId_);

			elementSetupHandlerId_ = 0;
		}

		// Stop further bus activity as teardown begins.
		if (GstBus* bus = gst_element_get_bus(pipeline)) {
			gst_bus_set_flushing(bus, TRUE);
			gst_object_unref(bus);
		}

		// A bus callback may already be running when flushing begins. Let it
		// finish before the worker changes or releases this pipeline.
		try { GlibLoop::instance().invokeAndWait([] {}); }
		catch (...) { LOG_WARNING("GStreamerVideo", "GLib bus barrier aborted during stop"); }

		const Uint64 stateStartNs = SDL_GetTicksNS();
		const GstStateChangeReturn ret =
			gst_element_set_state(pipeline, GST_STATE_NULL);
		const Uint64 stateMs = (SDL_GetTicksNS() - stateStartNs) / 1000000;
		if (stateMs > 500)
			LOG_WARNING("GStreamerVideo", "GST_STATE_NULL took " + std::to_string(stateMs) + " ms on control worker");

		if (ret == GST_STATE_CHANGE_FAILURE) {
			LOG_WARNING(
				"GStreamerVideo",
				"stop(): failed to set pipeline to GST_STATE_NULL");
		}
	}

	// It is now safe to make the pipeline unavailable to later callers.
	pipeline = std::exchange(
		pipeline_,
		nullptr);

	instantUriEnabled_ = false;

	GstElement* videoSink =
		std::exchange(
			videoSink_,
			nullptr);

	GstElement* audioSink =
		std::exchange(
			audioSink_,
			nullptr);

	const guint busWatchId =
		std::exchange(
			busWatchId_,
			0);

	if (busWatchId != 0) {
		try {
			GlibLoop::instance().invokeAndWait([busWatchId] {
				GMainContext* ctx = GlibLoop::instance().context();
				if (ctx) {
					if (GSource* source = g_main_context_find_source_by_id(ctx, busWatchId))
						g_source_destroy(source);
				}
			});
		} catch (...) { LOG_WARNING("GStreamerVideo", "GLib bus-watch removal aborted during stop"); }
	}

	if (videoSink) {
		detachAndDrainSink(
			videoSink,
			&padProbeId_);
	}

	if (audioSink) {
		detachAndDrainSink(
			audioSink,
			nullptr);
	}

	// Release our owning reference after NULL and bus-watch removal complete.
	if (pipeline) {
		gst_object_unref(pipeline);
	}

	{
		std::lock_guard<std::mutex> lock(sampleMutex_);

		if (stagedSample_.sample) {
			gst_sample_unref(
				stagedSample_.sample);

			stagedSample_.sample = nullptr;
		}

		stagedSample_.epoch = 0;
	}

	if (perspective_gva_) {
		g_value_array_free(
			perspective_gva_);

		perspective_gva_ = nullptr;
	}

	actualGstState_.store(
		GST_STATE_NULL,
		std::memory_order_release);

	hasVideoStream_.store(
		false,
		std::memory_order_release);

	isTextureReady_ = false;
	textureValid_.store(false, std::memory_order_release);
	presentationEpoch_.store(0, std::memory_order_release);

	auto self = shared_from_this();
	auto* completion = new std::shared_ptr<GStreamerVideo>(std::move(self));
	if (!SDL_RunOnMainThread([](void* data) {
		auto owner = std::unique_ptr<std::shared_ptr<GStreamerVideo>>(
			static_cast<std::shared_ptr<GStreamerVideo>*>(data));
		(*owner)->finishStopOnMain();
	}, completion, false)) {
		delete completion;
		LOG_ERROR("GStreamerVideo", "Could not schedule main-thread video teardown completion");
	}
}

void GStreamerVideo::finishStopOnMain() {
	if (videoSourceId_ != 0) {
		AudioBus::instance().setGain(audioHandle_, 0.0f);
		AudioBus::instance().removeSource(videoSourceId_);
		videoSourceId_ = 0;
		audioHandle_.reset();
	}
	destroyTextures();
	gpuInterop_.reset();
	lifecycle_.store(PipelineLifecycle::Idle, std::memory_order_release);
	releaseTransitionPermit("stop completed");
	pendingVideoStops.fetch_sub(1, std::memory_order_acq_rel);

	const std::string next = std::exchange(pendingOpenAfterStop_, {});
	if (!disposing_ && !next.empty()) {
		const bool cpuFallback = pendingOpenCpuFallback_;
		pendingOpenCpuFallback_ = false;
		if (!openMedia(next, cpuFallback))
			lifecycle_.store(PipelineLifecycle::Failed, std::memory_order_release);
	}
}
bool GStreamerVideo::isReadyForReuse() const {
	if (lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Stopping)
		return false;
	if (!pipeline_)
		return true;

	// The bus callback publishes Idle only after the retained playbin3 has
	// actually settled in PAUSED (preferred) or READY (fallback). Never wait
	// for a GStreamer state transition on the caller/render thread.
	return lifecycle_.load(std::memory_order_acquire) ==
		PipelineLifecycle::Idle;
}

bool GStreamerVideo::acquireStartupPermitOrQueue() {
    size_t activeTransitions = 0;
    bool acquired = false;
    bool newlyQueued = false;

    {
        std::lock_guard<std::mutex> lock(transitionSchedulerMutex_);

        auto& scheduler = transitionSchedulers_[monitor_];

        if (transitionPermitKind_ == TransitionPermitKind::Startup) {
            return true;
        }

        // A draining instance must not begin a new URI transition.
        if (transitionPermitKind_ == TransitionPermitKind::Drain) {
            return false;
        }

        if (scheduler.activeTransitions <
            MAX_CONCURRENT_TRANSITIONS_PER_MONITOR)
        {
            transitionPermitKind_ = TransitionPermitKind::Startup;
            ++scheduler.activeTransitions;
            activeTransitions = scheduler.activeTransitions;
            acquired = true;
        }
        else if (!startupPermitQueued_.exchange(
            true,
            std::memory_order_acq_rel))
        {
            scheduler.startupWaiters.push_back(weak_from_this());
            activeTransitions = scheduler.activeTransitions;
            newlyQueued = true;
        }
        else {
            activeTransitions = scheduler.activeTransitions;
        }
    }

    if (acquired) {
        LOG_DEBUG(
            "GStreamerVideo",
            "Transition permit acquired for startup: monitor=" +
            std::to_string(monitor_) +
            " active=" + std::to_string(activeTransitions) +
            "/" + std::to_string(MAX_CONCURRENT_TRANSITIONS_PER_MONITOR));
    }
    else if (newlyQueued) {
        LOG_DEBUG(
            "GStreamerVideo",
            "Transition permit deferred: monitor=" +
            std::to_string(monitor_) +
            " active=" + std::to_string(activeTransitions) +
            "/" + std::to_string(MAX_CONCURRENT_TRANSITIONS_PER_MONITOR));
    }

    return acquired;
}

void GStreamerVideo::cancelStartupPermitWait() {
    // Waiters are removed lazily from the scheduler deque. Clearing this flag
    // is enough to make a stale weak_ptr in that deque ineligible for grant.
    startupPermitQueued_.store(false, std::memory_order_release);
}

void GStreamerVideo::occupyDrainPermit() {
    cancelStartupPermitWait();

    size_t activeTransitions = 0;
    bool retainedStartupPermit = false;
    bool addedDrainPermit = false;

    {
        std::lock_guard<std::mutex> lock(transitionSchedulerMutex_);

        auto& scheduler = transitionSchedulers_[monitor_];

        if (transitionPermitKind_ == TransitionPermitKind::Startup) {
            // The URI switch was released/recycled before its first frame.
            // Keep the exact same slot occupied until PAUSED/READY settles.
            transitionPermitKind_ = TransitionPermitKind::Drain;
            retainedStartupPermit = true;
        }
        else if (transitionPermitKind_ == TransitionPermitKind::None) {
            // A Ready pipeline may be recycled after its startup permit has
            // already been released. Draining still consumes real GStreamer /
            // decoder work, so account for it even if this temporarily pushes
            // activeTransitions above the startup limit. New startups remain
            // blocked until enough drains settle.
            transitionPermitKind_ = TransitionPermitKind::Drain;
            ++scheduler.activeTransitions;
            addedDrainPermit = true;
        }

        activeTransitions = scheduler.activeTransitions;
    }

    if (retainedStartupPermit || addedDrainPermit) {
        LOG_DEBUG(
            "GStreamerVideo",
            std::string(retainedStartupPermit
                ? "Transition permit retained for drain: "
                : "Drain added to transition budget: ") +
            "monitor=" + std::to_string(monitor_) +
            " active=" + std::to_string(activeTransitions) +
            "/" + std::to_string(MAX_CONCURRENT_TRANSITIONS_PER_MONITOR));
    }
}

void GStreamerVideo::releaseTransitionPermit(const char* reason) {
    std::vector<std::shared_ptr<GStreamerVideo>> wake;
    size_t activeAfterRelease = 0;
    bool released = false;

    {
        std::lock_guard<std::mutex> lock(transitionSchedulerMutex_);

        auto it = transitionSchedulers_.find(monitor_);
        if (it == transitionSchedulers_.end()) {
            transitionPermitKind_ = TransitionPermitKind::None;
            startupPermitQueued_.store(false, std::memory_order_release);
            return;
        }

        auto& scheduler = it->second;

        if (transitionPermitKind_ != TransitionPermitKind::None) {
            transitionPermitKind_ = TransitionPermitKind::None;
            if (scheduler.activeTransitions > 0)
                --scheduler.activeTransitions;
            released = true;
        }

        // Fill all newly-available startup slots. Queue entries are weak and
        // self-invalidating, so cancelled/destroyed instances are simply skipped.
        while (scheduler.activeTransitions <
            MAX_CONCURRENT_TRANSITIONS_PER_MONITOR &&
            !scheduler.startupWaiters.empty())
        {
            auto weak = std::move(scheduler.startupWaiters.front());
            scheduler.startupWaiters.pop_front();

            auto next = weak.lock();
            if (!next)
                continue;

            if (!next->startupPermitQueued_.exchange(
                false,
                std::memory_order_acq_rel))
            {
                continue;
            }

            const PipelineLifecycle life =
                next->lifecycle_.load(std::memory_order_acquire);

            if (!next->pipeline_ ||
                life == PipelineLifecycle::Draining ||
                life == PipelineLifecycle::Stopping ||
                life == PipelineLifecycle::Failed)
            {
                continue;
            }

            next->transitionPermitKind_ = TransitionPermitKind::Startup;
            ++scheduler.activeTransitions;
            wake.push_back(std::move(next));
        }

        activeAfterRelease = scheduler.activeTransitions;

        if (scheduler.activeTransitions == 0 &&
            scheduler.startupWaiters.empty())
        {
            transitionSchedulers_.erase(it);
        }
    }

    if (released) {
        LOG_DEBUG(
            "GStreamerVideo",
            std::string("Transition permit released (") +
            (reason ? reason : "unspecified") +
            "): monitor=" + std::to_string(monitor_) +
            " active=" + std::to_string(activeAfterRelease) +
            "/" + std::to_string(MAX_CONCURRENT_TRANSITIONS_PER_MONITOR));
    }

    for (auto& next : wake) {
        LOG_DEBUG(
            "GStreamerVideo",
            "Transition permit granted from queue: monitor=" +
            std::to_string(next->monitor_));

        // The request may have been coalesced many times while waiting. The
        // URI pump re-reads desiredRetargetFile_ and therefore starts only the
        // latest target for this instance.
        next->scheduleUriPump();
    }
}

void GStreamerVideo::resetRetargetState() {
    std::lock_guard<std::mutex> lock(retargetMutex_);

    desiredRetargetFile_.clear();
    switchingRetargetFile_.clear();
    ++desiredRetargetRequestId_;
    switchingRetargetRequestId_ = 0;
    switchingRetargetEpoch_ = 0;
    retargetTaskQueued_ = false;
    retargetInFlight_ = false;
}

void GStreamerVideo::scheduleUriPump() {
    bool queueTask = false;

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        if (!retargetInFlight_ &&
            !retargetTaskQueued_ &&
            !startupPermitQueued_.load(std::memory_order_acquire) &&
            !desiredRetargetFile_.empty())
        {
            retargetTaskQueued_ = true;
            queueTask = true;
        }
    }

    if (!queueTask)
        return;

    std::weak_ptr<GStreamerVideo> weak = weak_from_this();

    enqueueControl([weak]() {
        auto self = weak.lock();
        if (self)
            self->runUriPumpOnControl();
    });
}

void GStreamerVideo::runUriPumpOnControl() {
    std::string targetFile;
    uint64_t requestId = 0;

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        // This queued pump has now been consumed.
        retargetTaskQueued_ = false;

        if (retargetInFlight_ ||
            desiredRetargetFile_.empty() ||
            !pipeline_ ||
            lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Draining ||
            lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Stopping)
        {
            return;
        }

        // Snapshot only for diagnostics while waiting for a permit. If the UI
        // changes selection before a permit becomes available, the second
        // lock below deliberately re-reads the newest desired target.
        targetFile = desiredRetargetFile_;
    }

    if (!acquireStartupPermitOrQueue()) {
        LOG_DEBUG(
            "GStreamerVideo",
            "URI switch waiting for transition permit: " + targetFile);
        return;
    }

    bool staleAfterPermit = false;

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        if (retargetInFlight_ ||
            desiredRetargetFile_.empty() ||
            !pipeline_ ||
            lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Draining ||
            lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Stopping)
        {
            staleAfterPermit = true;
        }
        else {
            // IMPORTANT: re-read the desired target after permit acquisition.
            // Rapid-scroll requests may have coalesced while this instance was
            // waiting in the global scheduler.
            targetFile = desiredRetargetFile_;
            requestId = desiredRetargetRequestId_;

            switchingRetargetFile_ = targetFile;
            switchingRetargetRequestId_ = requestId;
            switchingRetargetEpoch_ = 0;
            retargetInFlight_ = true;
        }
    }

    if (staleAfterPermit) {
        releaseTransitionPermit("URI pump became stale before start");
        return;
    }

    gchar* uri = gst_filename_to_uri(targetFile.c_str(), nullptr);
    if (!uri) {
        {
            std::lock_guard<std::mutex> lock(retargetMutex_);
            retargetInFlight_ = false;
            switchingRetargetFile_.clear();
            switchingRetargetRequestId_ = 0;
            switchingRetargetEpoch_ = 0;
        }

        publishLifecycleUnlessStopping(PipelineLifecycle::Failed);
        LOG_ERROR(
            "GStreamerVideo",
            "Could not convert video path to URI: " + targetFile);
        releaseTransitionPermit("URI conversion failed");
        return;
    }

    const std::string uriText(uri);
    g_free(uri);

    const GstState actualState =
        actualGstState_.load(std::memory_order_acquire);

    const PipelineLifecycle lifeBeforeSwitch =
        lifecycle_.load(std::memory_order_acquire);

    const bool preserveReady =
        lifeBeforeSwitch == PipelineLifecycle::Ready;

    const bool canInstantSwitch =
        instantUriEnabled_ &&
        (actualState == GST_STATE_PAUSED ||
         actualState == GST_STATE_PLAYING);

    const bool needsPrerollState =
        actualState == GST_STATE_NULL ||
        actualState == GST_STATE_READY;

    /*
     * Close the sample/callback gate BEFORE touching uri.
     *
     * cbCtx_->epoch deliberately remains on the old generation while
     * playbackEpoch_ advances. Any old callback already in flight, and any
     * callback that arrives during g_object_set("uri"), therefore fails the
     * normal epoch comparison instead of being mislabeled as the new media.
     */
    const uint64_t newEpoch = nextUniquePlaybackEpoch_++;
    playbackEpoch_.store(newEpoch, std::memory_order_release);

    startupWorkerNs_.store(SDL_GetTicksNS(), std::memory_order_release);

    if (canInstantSwitch) {
        g_object_set(
            pipeline_,
            "instant-uri", TRUE,
            nullptr);
    }

    g_object_set(
        pipeline_,
        "uri", uriText.c_str(),
        nullptr);

    if (canInstantSwitch) {
        g_object_set(
            pipeline_,
            "instant-uri", FALSE,
            nullptr);
    }

    /*
     * Anything staged before the URI write belongs to the previous stream.
     * A callback that crossed the boundary concurrently still carries the old
     * callback epoch and updateFrame() will reject it against newEpoch.
     */
    {
        std::lock_guard<std::mutex> lock(sampleMutex_);

        if (stagedSample_.sample) {
            gst_sample_unref(stagedSample_.sample);
            stagedSample_.sample = nullptr;
        }

        stagedSample_.epoch = 0;
    }

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        // stop()/unload() may have invalidated the request while g_object_set()
        // was executing. Do not reopen the callback gate in that case.
        if (!retargetInFlight_ ||
            switchingRetargetRequestId_ != requestId ||
            switchingRetargetFile_ != targetFile ||
            !pipeline_ ||
            lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Draining ||
            lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Stopping)
        {
            return;
        }

        switchingRetargetEpoch_ = newEpoch;
    }

    // From this point on, only a successfully copied sample tagged with this
    // committed URI epoch may become presentation-valid.
    presentationEpoch_.store(newEpoch, std::memory_order_release);

    if (!preserveReady) {
        awaitingInitialPreroll_.store(true, std::memory_order_release);
        if (!publishLifecycleUnlessStopping(PipelineLifecycle::Starting)) return;
        playbackState_.store(PlaybackState::Paused, std::memory_order_release);

        // A pooled/cold instance has no presentation frame worth retaining.
        isTextureReady_ = false;
        dimensions_.store({ -1, -1 }, std::memory_order_release);
    }
    else {
        // Keep the warm allocation/pipeline, but never present pixels from the
        // previous URI while the new URI is waiting for its first copied frame.
        awaitingInitialPreroll_.store(false, std::memory_order_release);
    }

    /*
     * Open the callback gate LAST. From this point onward a sample can only be
     * tagged with newEpoch after the URI write/flush has completed.
     */
    if (cbCtx_) {
        cbCtx_->epoch.store(newEpoch, std::memory_order_release);
    }

    LOG_DEBUG(
        "GStreamerVideo",
        std::string(canInstantSwitch
            ? "URI switch started (instant): "
            : "URI switch started (cold/warm-preroll): ") +
        targetFile);

    if (!needsPrerollState)
        return;

    const GstStateChangeReturn ret =
        gst_element_set_state(
            pipeline_,
            GST_STATE_PAUSED);

    if (ret != GST_STATE_CHANGE_FAILURE)
        return;

    LOG_ERROR(
        "GStreamerVideo",
        "Failed to preroll URI: " + targetFile);

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);
        if (switchingRetargetRequestId_ == requestId) {
            retargetInFlight_ = false;
            switchingRetargetFile_.clear();
            switchingRetargetRequestId_ = 0;
            switchingRetargetEpoch_ = 0;
        }
    }

    awaitingInitialPreroll_.store(false, std::memory_order_release);

    const bool retryGL = glPipelineActive_.exchange(false);
    publishLifecycleUnlessStopping(
        retryGL ? PipelineLifecycle::Starting : PipelineLifecycle::Failed);

    releaseTransitionPermit("preroll state change failed");

    if (retryGL)
        pendingCpuFallback_.store(true, std::memory_order_release);
}

bool GStreamerVideo::acceptRetargetVideoSample(uint64_t epoch) {
    bool superseded = false;
    bool queueNext = false;
    std::string completedFile;
    std::string desiredFile;

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        // Ordinary steady-state frame, or a frame from a generation that was
        // not started by the URI pump.
        if (!retargetInFlight_ ||
            switchingRetargetEpoch_ != epoch)
        {
            return true;
        }

        completedFile = switchingRetargetFile_;
        desiredFile = desiredRetargetFile_;

        superseded =
            desiredRetargetRequestId_ != switchingRetargetRequestId_ ||
            desiredRetargetFile_ != switchingRetargetFile_;

        retargetInFlight_ = false;
        switchingRetargetFile_.clear();
        switchingRetargetRequestId_ = 0;
        switchingRetargetEpoch_ = 0;

        if (superseded &&
            !retargetTaskQueued_ &&
            !desiredRetargetFile_.empty())
        {
            retargetTaskQueued_ = true;
            queueNext = true;
        }
    }

    // The first valid video sample marks the end of this URI startup. Release
    // the real GStreamer transition slot before a superseded target queues its
    // next attempt, allowing another waiting instance to make progress fairly.
    releaseTransitionPermit("first accepted video sample");

    if (superseded) {
        /*
         * Close the gate again immediately. The just-completed URI is already
         * obsolete, so do not allow subsequent frames from it to overwrite the
         * presentation frame while the latest URI request is waiting on GLib.
         */
        const uint64_t gateEpoch = nextUniquePlaybackEpoch_++;
        playbackEpoch_.store(gateEpoch, std::memory_order_release);

        LOG_DEBUG(
            "GStreamerVideo",
            "URI switch first frame superseded: " + completedFile +
            " -> " + desiredFile);

        if (queueNext) {
            std::weak_ptr<GStreamerVideo> weak = weak_from_this();
            enqueueControl([weak]() {
                auto self = weak.lock();
                if (self)
                    self->runUriPumpOnControl();
            });
        }

        return false;
    }

    LOG_DEBUG(
        "GStreamerVideo",
        "URI switch accepted first frame: " + completedFile);

    return true;
}

bool GStreamerVideo::acceptRetargetAudioOnly(uint64_t epoch) {
    bool superseded = false;
    bool queueNext = false;
    std::string completedFile;
    std::string desiredFile;

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        if (!retargetInFlight_ ||
            switchingRetargetEpoch_ != epoch)
        {
            return true;
        }

        completedFile = switchingRetargetFile_;
        desiredFile = desiredRetargetFile_;

        superseded =
            desiredRetargetRequestId_ != switchingRetargetRequestId_ ||
            desiredRetargetFile_ != switchingRetargetFile_;

        retargetInFlight_ = false;
        switchingRetargetFile_.clear();
        switchingRetargetRequestId_ = 0;
        switchingRetargetEpoch_ = 0;

        if (superseded &&
            !retargetTaskQueued_ &&
            !desiredRetargetFile_.empty())
        {
            retargetTaskQueued_ = true;
            queueNext = true;
        }
    }

    releaseTransitionPermit("accepted audio-only/delayed-video startup");

    if (superseded) {
        const uint64_t gateEpoch = nextUniquePlaybackEpoch_++;
        playbackEpoch_.store(gateEpoch, std::memory_order_release);

        LOG_DEBUG(
            "GStreamerVideo",
            "URI switch audio-only superseded: " + completedFile +
            " -> " + desiredFile);

        if (queueNext) {
            std::weak_ptr<GStreamerVideo> weak = weak_from_this();
            enqueueControl([weak]() {
                auto self = weak.lock();
                if (self)
                    self->runUriPumpOnControl();
            });
        }

        return false;
    }

    if (!publishLifecycleUnlessStopping(PipelineLifecycle::Ready)) return false;

    LOG_DEBUG(
        "GStreamerVideo",
        "URI switch accepted audio-only media: " + completedFile);

    return true;
}

bool GStreamerVideo::shouldDiscardRetargetAudio(uint64_t epoch) const {
    std::lock_guard<std::mutex> lock(retargetMutex_);

    return retargetInFlight_ &&
        switchingRetargetEpoch_ == epoch;
}

bool GStreamerVideo::prepareForRetarget() {
    /*
     * An in-place same-list recycle no longer drives playbin3 through PAUSED
     * or waits in gst_element_get_state(). playbin3 instant-uri performs its
     * own old-pad flush/block and source activation. The actual URI write is
     * serialized/coalesced by the URI pump in openMedia().
     *
     * Keep the warm texture allocation and playbin graph, but invalidate the
     * old pixels immediately. They must not be presented as belonging to the
     * next URI; updateFrame() revalidates only after copying a frame from the
     * newly committed playback epoch.
     */
    pendingCpuFallback_.store(false, std::memory_order_release);
    textureValid_.store(false, std::memory_order_release);
    presentationEpoch_.store(0, std::memory_order_release);
#ifdef _WIN32
    if (gpuInterop_) gpuInterop_->invalidateFrame();
#endif

    if (!initialized_)
        return false;

    if (!pipeline_)
        return true;

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
        "Prepared active pipeline for coalesced instant-uri retarget "
        "(no PAUSED state transition).");

    return true;
}

bool GStreamerVideo::unload() {
	pendingCpuFallback_.store(false, std::memory_order_release);

	if (gpuInterop_) {
		gpuInterop_->discardFrames();
	}

	cancelStartupPermitWait();
	resetRetargetState();

	if (!initialized_) {
		releaseTransitionPermit("unload while uninitialized");
		return false;
	}

	const PipelineLifecycle life =
		lifecycle_.load(std::memory_order_acquire);

	// A second release request while the retained pipeline is already
	// settling has nothing useful to do. Most importantly, do not wait.
	if (life == PipelineLifecycle::Draining || life == PipelineLifecycle::Stopping)
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
	textureValid_.store(false, std::memory_order_release);
	presentationEpoch_.store(0, std::memory_order_release);

	dimensions_.store(
		{ -1, -1 },
		std::memory_order_release);

	if (videoSink_ && GST_IS_APP_SINK(videoSink_)) {
		guint64 inFrames = 0, outFrames = 0, droppedFrames = 0;
		g_object_get(videoSink_,
			"in", &inFrames,
			"out", &outFrames,
			"dropped", &droppedFrames,
			nullptr);
		if (inFrames > 0 || droppedFrames > 0) {
			LOG_DEBUG("GStreamerVideo",
				"Appsink stats on unload (" + currentFile_ + "): in=" + std::to_string(inFrames) +
				" out=" + std::to_string(outFrames) +
				" dropped=" + std::to_string(droppedFrames));
		}
	}

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

		releaseTransitionPermit("unload without pipeline");
		return true;
	}

	// From this point until PAUSED/READY actually settles, the retained pipeline
	// represents real GStreamer transition work. If a startup permit was already
	// held it is converted to Drain; otherwise the drain is added to the global
	// per-monitor budget even if that temporarily exceeds the startup limit.
	occupyDrainPermit();

	// Serialize the retained-pipeline state request on this instance's control
	// queue. The dead epoch above
	// invalidates older queued commands before this one executes.
	const uint64_t drainEpoch =
		playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, drainEpoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != drainEpoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Draining ||
			!self->pipeline_)
			return;

		GstStateChangeReturn ret =
			gst_element_set_state(
				self->pipeline_,
				GST_STATE_PAUSED);

		if (ret == GST_STATE_CHANGE_FAILURE) {
			LOG_WARNING(
				"GStreamerVideo",
				"unload(): PAUSED request failed; falling back asynchronously to READY.");

			ret = gst_element_set_state(
				self->pipeline_,
				GST_STATE_READY);

			if (ret == GST_STATE_CHANGE_FAILURE) {
				self->lifecycle_.store(
					PipelineLifecycle::Failed,
					std::memory_order_release);

				LOG_ERROR(
					"GStreamerVideo",
					"unload(): READY fallback request also failed.");

				self->releaseTransitionPermit(
					"unload READY fallback failed");
				return;
			}

			if (ret == GST_STATE_CHANGE_SUCCESS ||
				ret == GST_STATE_CHANGE_NO_PREROLL)
			{
				self->actualGstState_.store(
					GST_STATE_READY,
					std::memory_order_release);
				self->lifecycle_.store(
					PipelineLifecycle::Idle,
					std::memory_order_release);

				self->releaseTransitionPermit(
					"unload settled synchronously in READY");
			}
			return;
		}

		if (ret == GST_STATE_CHANGE_SUCCESS ||
			ret == GST_STATE_CHANGE_NO_PREROLL)
		{
			self->actualGstState_.store(
				GST_STATE_PAUSED,
				std::memory_order_release);
			self->lifecycle_.store(
				PipelineLifecycle::Idle,
				std::memory_order_release);

			self->releaseTransitionPermit(
				"unload settled synchronously in PAUSED");
		}

		// GST_STATE_CHANGE_ASYNC intentionally leaves lifecycle_ == Draining.
		// STATE_CHANGED will publish Idle when PAUSED has actually settled.
	});

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

	// Detect instant-uri support once, but do not leave it permanently enabled.
	// Each warm retarget enables it only for the URI property write itself.
	// This follows playbin3/uridecodebin3's intended immediate-switch contract
	// and prevents unrelated URI writes from implicitly becoming instant switches.
	instantUriEnabled_ =
		g_object_class_find_property(
			G_OBJECT_GET_CLASS(pipeline_),
			"instant-uri") != nullptr;

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
		"max-buffers", 16,
		"leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM,
		"qos", FALSE,
		"sync", TRUE,
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
	SDL_AudioFormat fmt =
		AudioBus::instance().dev_fmt();
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
		"max-buffers", 1,
		"leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM,
		"qos", TRUE,
		"max-lateness", gint64(25 * GST_MSECOND),
		"sync", TRUE,
		"enable-last-sample", FALSE,
		"wait-on-eos", FALSE,
		nullptr);

	GstAppSinkCallbacks videoCbs = {};
	videoCbs.new_preroll = &GStreamerVideo::on_new_preroll;
	videoCbs.new_sample = &GStreamerVideo::on_new_sample;
	videoCbs.propose_allocation = &GStreamerVideo::on_propose_allocation;

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
			glPipelineActive_.store(true);
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
				? gpuInterop_->caps()
				: "video/x-raw,format=(string)NV12");
			sdlFormat_ = gpuInterop_ && gpuInterop_->available() ? gpuInterop_->pixelFormat() : SDL_PIXELFORMAT_NV12;
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

    if (lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Stopping) {
        pendingOpenAfterStop_ = file;
        pendingOpenCpuFallback_ = cpuFallback;
        currentFile_ = file;
        return true;
    }

    if (lifecycle_.load(std::memory_order_acquire) ==
        PipelineLifecycle::Draining)
    {
        LOG_DEBUG(
            "GStreamerVideo",
            "open(): rejected while pipeline is draining: " + file);
        return false;
    }

    /*
     * A real fallback rebuild still starts from a clean pipeline. Ordinary
     * media changes, however, stay on the retained playbin3 and are routed
     * through the coalescing URI pump below.
     */
    if (!cpuFallback && disableInterop_) {
        disableInterop_ = false;
		pendingOpenAfterStop_ = file;
		pendingOpenCpuFallback_ = false;
		return beginStop(true);
    }

    const bool hadPipeline = pipeline_ != nullptr;

    currentFile_ = file;

    // A logical media change immediately invalidates the previously displayed
    // pixels. The underlying texture allocation may remain warm/reusable, but
    // no frame may be exposed again until runUriPumpOnControl() commits a new
    // playback epoch and updateFrame() copies a sample from that epoch.
    textureValid_.store(false, std::memory_order_release);
    presentationEpoch_.store(0, std::memory_order_release);
#ifdef _WIN32
    if (gpuInterop_) gpuInterop_->invalidateFrame();
#endif

    startupOpenedNs_ = SDL_GetTicksNS();
    startupWorkerNs_.store(0, std::memory_order_release);
    startupSampleNs_.store(0, std::memory_order_release);
    startupLogged_ = false;

    loggedGpu_ = false;
    loggedUpload_ = false;
    loopsFinished_.store(false, std::memory_order_release);

    if (!createPipelineIfNeeded())
        return false;

    /*
     * First/cold opens have no previous texture resource. Warm in-place
     * retargets keep isTextureReady_/dimensions_ untouched so their allocation
     * can be reused, while textureValid_ above prevents stale pixels from being
     * presented during the handoff.
     */
    if (!hadPipeline) {
        isTextureReady_ = false;
        dimensions_.store({ -1, -1 }, std::memory_order_release);
        hasVideoStream_.store(false, std::memory_order_release);
        playbackState_.store(PlaybackState::Paused, std::memory_order_release);
    }

    startupInstantSwitch_ =
        hadPipeline &&
        instantUriEnabled_ &&
        (actualGstState_.load(std::memory_order_acquire) == GST_STATE_PAUSED ||
         actualGstState_.load(std::memory_order_acquire) == GST_STATE_PLAYING);

    bool coalesced = false;

    {
        std::lock_guard<std::mutex> lock(retargetMutex_);

        desiredRetargetFile_ = file;
        ++desiredRetargetRequestId_;

        coalesced =
            retargetInFlight_ ||
            retargetTaskQueued_ ||
            startupPermitQueued_.load(std::memory_order_acquire);
    }

    if (coalesced) {
        LOG_DEBUG(
            "GStreamerVideo",
            "Coalesced URI request; latest target is now: " + file);
    }

    scheduleUriPump();

    if (videoSourceId_ == 0) {
        videoSourceId_ =
            AudioBus::instance().addSource("video-preview");
        audioHandle_ =
            AudioBus::instance().getHandle(videoSourceId_);
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
	gpointer data) {
	const auto* self = static_cast<GStreamerVideo*>(data);
	const bool isHw = Configuration::HardwareVideoAccel && (!self || !self->disableInterop_);
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


	if (isHw && GST_IS_VIDEO_DECODER(element)) {
		if (auto* factory = gst_element_get_factory(element)) {
			LOG_INFO(
				"GStreamerVideo",
				std::string("playbin3 created video decoder: ") +
				GST_OBJECT_NAME(factory) +
				" instance=" + std::to_string(reinterpret_cast<uintptr_t>(element)));
		}
	}

	if (!isHw &&
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
	if (!publishLifecycleUnlessStopping(PipelineLifecycle::Ready)) return;
}

GstFlowReturn GStreamerVideo::on_new_preroll(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx) return GST_FLOW_OK;

	GstSample* s = gst_app_sink_pull_preroll(sink);
	if (!s) return GST_FLOW_OK;

	auto video = ctx->self.load().lock();
	if (!video) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	const uint64_t callbackEpoch = ctx->epoch.load(std::memory_order_acquire);

	// STRICT EPOCH VALIDATION: Drop stale frames instantly.
	if (callbackEpoch != video->playbackEpoch_.load(std::memory_order_acquire)) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	if (!video->acceptRetargetVideoSample(callbackEpoch)) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

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

	GstSample* oldPreroll = nullptr;
	{
		std::lock_guard<std::mutex> lock(video->sampleMutex_);
		oldPreroll = std::exchange(video->stagedSample_.sample, s);
		video->stagedSample_.epoch = callbackEpoch;
	}
	if (oldPreroll) {
		gst_sample_unref(oldPreroll);
	}

	video->completeInitialPreroll(callbackEpoch);
	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx) return GST_FLOW_OK;

	GstSample* s = gst_app_sink_pull_sample(sink);
	if (!s) return GST_FLOW_OK;

	auto video = ctx->self.load().lock();
	if (!video) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	const uint64_t callbackEpoch = ctx->epoch.load(std::memory_order_acquire);

	if (callbackEpoch != video->playbackEpoch_.load(std::memory_order_acquire)) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	if (!video->acceptRetargetVideoSample(callbackEpoch)) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

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

	GstSample* oldSample = nullptr;
	{
		std::lock_guard<std::mutex> lock(video->sampleMutex_);
		oldSample = std::exchange(video->stagedSample_.sample, s);
		video->stagedSample_.epoch = callbackEpoch;
	}
	if (oldSample) {
		gst_sample_unref(oldSample);
	}

	video->completeInitialPreroll(callbackEpoch);
	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_audio_new_sample(
	GstAppSink* sink,
	gpointer user_data) {
	auto* ctx =
		static_cast<CallbackCtx*>(
			user_data);

	if (!ctx)
		return GST_FLOW_OK;

	GstSample* sample =
		gst_app_sink_pull_sample(
			sink);

	if (!sample)
		return GST_FLOW_OK;

	auto video =
		ctx->self.load().lock();

	if (!video) {
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	const uint64_t callbackEpoch =
		ctx->epoch.load(
			std::memory_order_acquire);

	if (callbackEpoch !=
		video->playbackEpoch_.load(
			std::memory_order_acquire))
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	// Do not let audio from a URI transition get ahead of the first accepted
	// video frame. If that URI has already been superseded, the next URI pump
	// will close the epoch gate before any of its audio becomes presentation-current.
	// If 500ms has elapsed since startup worker started and no video sample arrived,
	// treat this as audio-only or delayed-video media to unblock audio and release the permit.
	if (video->shouldDiscardRetargetAudio(callbackEpoch)) {
		const Uint64 start = video->startupWorkerNs_.load(std::memory_order_acquire);
		const Uint64 now = SDL_GetTicksNS();
		if (start > 0 && (now - start > 500000000ULL)) {
			if (!video->acceptRetargetAudioOnly(callbackEpoch)) {
				gst_sample_unref(sample);
				return GST_FLOW_OK;
			}
		} else {
			gst_sample_unref(sample);
			return GST_FLOW_OK;
		}
	}

	/*
	 * Audio produced during startup/draining is pulled and discarded so the
	 * appsink itself never backs up.
	 */
	const bool active =
		video->audioHandle_ &&
		video->lifecycle_.load(
			std::memory_order_acquire) ==
		PipelineLifecycle::Ready;

	if (!active) {
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	GstBuffer* buffer =
		gst_sample_get_buffer(
			sample);

	if (!buffer ||
		GST_BUFFER_FLAG_IS_SET(
			buffer,
			GST_BUFFER_FLAG_CORRUPTED))
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	/*
	 * A new playback epoch is a semantic discontinuity even if GStreamer
	 * doesn't mark its first output buffer DISCONT.
	 */
	const uint64_t previousEpoch =
		video->lastFadedEpoch_.exchange(
			callbackEpoch,
			std::memory_order_acq_rel);

	const bool firstForEpoch =
		previousEpoch !=
		callbackEpoch;

	/*
	 * GStreamer marks the first buffer after flushing seeks, loops and
	 * other timeline discontinuities with DISCONT.
	 */
	const bool discontinuity =
		GST_BUFFER_FLAG_IS_SET(
			buffer,
			GST_BUFFER_FLAG_DISCONT);

	if (firstForEpoch ||
		discontinuity)
	{
		/*
		 * Any queued data belongs to the old timeline.
		 */
		AudioBus::instance().clear(
			video->audioHandle_);

		AudioBus::instance().triggerFadeIn(
			video->audioHandle_);
	}

	GstMapInfo map{};

	if (gst_buffer_map(
		buffer,
		&map,
		GST_MAP_READ))
	{
		/*
		 * Caps guarantee this is native-endian interleaved F32 matching
		 * AudioBus' rate and channel count.
		 */
		AudioBus::instance().push(
			video->audioHandle_,
			map.data,
			static_cast<int>(
				map.size));

		gst_buffer_unmap(
			buffer,
			&map);
	}

	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

gboolean GStreamerVideo::on_propose_allocation(
	GstAppSink* /*sink*/,
	GstQuery* query,
	gpointer user_data)
{
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx) return FALSE;
	auto video = ctx->self.load().lock();
	if (!video || !video->gpuInterop_) return FALSE;
	return video->gpuInterop_->proposeAllocation(query) ? TRUE : FALSE;
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
	textureValid_.store(false, std::memory_order_release);
}

void GStreamerVideo::updateFrame() {
	if (pendingCpuFallback_.exchange(false, std::memory_order_acq_rel)) {
		const auto file = currentFile_;
		disableInterop_ = true;
		pendingOpenAfterStop_ = file;
		pendingOpenCpuFallback_ = true;
		beginStop(true);
		return;
	}
	GstSample* sampleToProcess = nullptr;
	uint64_t sampleEpoch = 0;

	{
		std::lock_guard<std::mutex> lock(sampleMutex_);
		if (stagedSample_.sample && stagedSample_.epoch == playbackEpoch_.load(std::memory_order_acquire)) {
			sampleToProcess = stagedSample_.sample;
			sampleEpoch = stagedSample_.epoch;
			stagedSample_.sample = nullptr; // Consume!
			stagedSample_.epoch = 0;
			// No state mutation here whatsoever. The rendering system only consumes frames.
		}
	}

	if (!sampleToProcess) return;

	// A new media request may have invalidated presentation before the GLib URI
	// pump commits its next epoch. Never let an already-staged old frame make the
	// texture valid again during that gap.
	if (sampleEpoch == 0 ||
		sampleEpoch != presentationEpoch_.load(std::memory_order_acquire) ||
		sampleEpoch != playbackEpoch_.load(std::memory_order_acquire))
	{
		gst_sample_unref(sampleToProcess);
		return;
	}

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
			textureValid_.store(true, std::memory_order_release);
			logStartupTiming();
			if (!loggedGpu_) {
				LOG_INFO("GStreamerVideo", std::string("GPU texture interop ACTIVE: ") + gpuInterop_->description() + "; monitor " + std::to_string(monitor_) + "; " + currentFile_);
				loggedGpu_ = true;
			}
			gst_sample_unref(sampleToProcess);
			return;
		}
#ifdef _WIN32
		if (gpuInterop_->deferred()) { gst_sample_unref(sampleToProcess); return; }
#endif
		if (!loggedUpload_) LOG_INFO("GStreamerVideo", std::string("GPU texture interop fallback: ") + gpuInterop_->reason() + "; " + currentFile_);
#ifdef RETROFE_HAVE_EGL_DMABUF
		// Never CPU-map a tiled DMA_DRM frame as ordinary NV12/RGBA. Reopen
		// with system-memory negotiation through the existing recovery path.
		isTextureReady_ = false;
		textureValid_.store(false, std::memory_order_release);
		presentationEpoch_.store(0, std::memory_order_release);
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
		textureValid_.store(true, std::memory_order_release);
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

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			!self->pipeline_)
			return;

		gint64 currentPos = 0, duration = 0;
		if (!gst_element_query_position(self->pipeline_, GST_FORMAT_TIME, &currentPos) ||
			!gst_element_query_duration(self->pipeline_, GST_FORMAT_TIME, &duration))
			return;

		gint64 newPos = currentPos + 60 * GST_SECOND;
		if (newPos > duration)
			newPos = std::max<gint64>(0, duration - GST_SECOND / 10);

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, newPos,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	});
}

void GStreamerVideo::skipBackward() {
	if (!isPipelineReady() || !pipeline_) return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			!self->pipeline_)
			return;

		gint64 currentPos = 0;
		if (!gst_element_query_position(self->pipeline_, GST_FORMAT_TIME, &currentPos))
			return;

		const gint64 newPos =
			(currentPos > 60 * GST_SECOND) ? currentPos - 60 * GST_SECOND : 0;

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, newPos,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	});
}

void GStreamerVideo::skipForwardp() {
	if (!isPipelineReady() || !pipeline_) return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			!self->pipeline_)
			return;

		gint64 currentPos = 0, duration = 0;
		if (!gst_element_query_position(self->pipeline_, GST_FORMAT_TIME, &currentPos) ||
			!gst_element_query_duration(self->pipeline_, GST_FORMAT_TIME, &duration))
			return;

		const gint64 skipAmount = duration / 20;
		gint64 newPos = currentPos + skipAmount;
		if (newPos > duration)
			newPos = std::max<gint64>(0, duration - GST_SECOND / 10);

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, newPos,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	});
}

void GStreamerVideo::skipBackwardp() {
	if (!isPipelineReady() || !pipeline_) return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			!self->pipeline_)
			return;

		gint64 currentPos = 0, duration = 0;
		if (!gst_element_query_position(self->pipeline_, GST_FORMAT_TIME, &currentPos) ||
			!gst_element_query_duration(self->pipeline_, GST_FORMAT_TIME, &duration))
			return;

		const gint64 skipAmount = duration / 20;
		const gint64 newPos = (currentPos > skipAmount) ? currentPos - skipAmount : 0;

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, newPos,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	});
}

void GStreamerVideo::pause() {
	if (!pipeline_ ||
		lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready)
		return;

	if (playbackState_.exchange(
			PlaybackState::Paused,
			std::memory_order_acq_rel) == PlaybackState::Paused)
		return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			self->playbackState_.load(std::memory_order_acquire) != PlaybackState::Paused ||
			!self->pipeline_)
			return;

		gst_element_set_state(self->pipeline_, GST_STATE_PAUSED);
	});
}

void GStreamerVideo::resume() {
	if (!pipeline_ ||
		lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready)
		return;

	if (playbackState_.exchange(
			PlaybackState::Playing,
			std::memory_order_acq_rel) == PlaybackState::Playing)
		return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			self->playbackState_.load(std::memory_order_acquire) != PlaybackState::Playing ||
			!self->pipeline_)
			return;

		gst_element_set_state(self->pipeline_, GST_STATE_PLAYING);
	});
}

void GStreamerVideo::restart() {
	if (!isPipelineReady() || !pipeline_) return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			!self->pipeline_)
			return;

		gint64 currentPos = 0;
		if (gst_element_query_position(self->pipeline_, GST_FORMAT_TIME, &currentPos) &&
			currentPos < GST_SECOND / 10)
			return;

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, 0,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	});
}

void GStreamerVideo::rewindAndPause() {
	if (!pipeline_ ||
		lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready)
		return;

	playbackState_.store(PlaybackState::Paused, std::memory_order_release);

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			self->playbackState_.load(std::memory_order_acquire) != PlaybackState::Paused ||
			!self->pipeline_)
			return;

		gst_element_set_state(self->pipeline_, GST_STATE_PAUSED);

		if (self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			self->playbackState_.load(std::memory_order_acquire) != PlaybackState::Paused ||
			!self->pipeline_)
			return;

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			GST_SEEK_TYPE_SET, 0,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	});
}

void GStreamerVideo::loop() {
	if (!isPipelineReady() || !pipeline_) return;

	const uint64_t epoch = playbackEpoch_.load(std::memory_order_acquire);
	std::weak_ptr<GStreamerVideo> weak = weak_from_this();

	enqueueControl([weak, epoch]() {
		auto self = weak.lock();
		if (!self ||
			self->playbackEpoch_.load(std::memory_order_acquire) != epoch ||
			self->lifecycle_.load(std::memory_order_acquire) != PipelineLifecycle::Ready ||
			!self->pipeline_)
			return;

		gst_element_seek(
			self->pipeline_, 1.0, GST_FORMAT_TIME,
			GST_SEEK_FLAG_FLUSH,
			GST_SEEK_TYPE_SET, 0,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
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
