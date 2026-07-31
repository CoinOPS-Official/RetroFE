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
#pragma once

#include "../Database/Configuration.h"
#include "../SDL.h"
#include "../Utility/Utils.h"
#include "../Utility/Log.h"
#include "../Sound/AudioBus.h" 
#include "IVideo.h"
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

extern "C" {
#if (__APPLE__)
#if __has_include(<gstreamer-1.0/gst/gst.h>)
#include <gstreamer-1.0/gst/gst.h>
#include <gstreamer-1.0/gst/video/video.h>
#include <gstreamer-1.0/gst/app/gstappsink.h>
#elif __has_include(<GStreamer/gst/gst.h>)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/video.h>
#include <GStreamer/gst/app/gstappsink.h>
#else
#error "Cannot find Gstreamer headers"
#endif
#else
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#endif
}

// Unified State Machine Enum
enum class PipelineLifecycle {
    Idle,      // Not currently processing a file
    Starting,  // URI set, waiting for Preroll
    Ready,     // Preroll complete, ready to stream data
    Draining,  // Returning the pipeline to READY before pool reuse
    Failed     // Hard failure
};

enum class PlaybackState {
    None,
    Playing,
    Paused
};

// Strict Epoch-Tagged Frame Consumer
struct TaggedSample {
    GstSample* sample = nullptr;
    uint64_t epoch = 0;
};

class GStreamerVideo final : public IVideo,
    public std::enable_shared_from_this<GStreamerVideo> {

public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo&) = delete;
    GStreamerVideo& operator=(const GStreamerVideo&) = delete;
    ~GStreamerVideo() override;

    // --- Interface methods ---
    bool initialize() override;
    bool deInitialize() override;
    bool unload() override;
    bool createPipelineIfNeeded();
    bool open(const std::string& file) override; // Renamed from play
    bool stop() override;
    bool isReadyForReuse() const;
    SDL_Texture* getTexture() const override;
    void updateFrame() override; // Renamed from draw
    void setNumLoops(int n);
    bool isPlaying() override;
    void setVolume(float volume) override;
    VideoDim getDimensions() override;
    void skipForward() override;
    void skipBackward() override;
    void skipForwardp() override;
    void skipBackwardp() override;
    void pause() override;
    void resume() override;
    void restart() override;
    void loop() override;
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;
    void setSoftOverlay(bool value) override;
    void setPerspectiveCorners(const int* corners);
    bool hasVideoStream() const override { return hasVideoStream_; }
    bool hasFinishedLoops() const override;

    // --- Mapped IVideo Interface ---
    bool hasError() const override {
        return lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Failed;
    }

    IVideo::VideoState getTargetState() const override {
        PlaybackState s = playbackState_.load(std::memory_order_acquire);
        if (s == PlaybackState::Playing) return IVideo::VideoState::Playing;
        if (s == PlaybackState::Paused) return IVideo::VideoState::Paused;
        return IVideo::VideoState::None;
    }

    IVideo::VideoState getActualState() const override;

    bool isPipelineReady() const override {
        return lifecycle_.load(std::memory_order_acquire) == PipelineLifecycle::Ready;
    }

    VideoSnapshot getSnapshot() const override;

    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);

private:
    // --- Callback context to avoid UAF in GStreamer/GLib callbacks ---
    struct CallbackCtx {
        grefcount ref;
        std::atomic<std::weak_ptr<GStreamerVideo>> self;
        std::atomic<uint64_t> epoch{ 0 };
    };

    CallbackCtx* cbCtx_ = nullptr;
    static CallbackCtx* cbCtxRef(CallbackCtx* c);
    static void cbCtxUnref(gpointer data);

    // === Core Thread-Safe State & Epoch Validation ===
    // Protects raw GStreamer pointers and state-changing operations reached
    // from both the main thread and GLib callbacks.
    mutable std::recursive_mutex pipelineMutex_;
    std::atomic<PipelineLifecycle> lifecycle_{ PipelineLifecycle::Idle };
    std::atomic<PlaybackState> playbackState_{ PlaybackState::None };
    std::atomic<uint64_t> playbackEpoch_{ 0 };
    static std::atomic<uint64_t> nextUniquePlaybackEpoch_;

    // The "Truth": Updated via GST_MESSAGE_STATE_CHANGED
    std::atomic<GstState> actualGstState_{ GST_STATE_NULL };
    static IVideo::VideoState mapGstState(GstState state);

    // === Hardware Budget / CPU Preroll Gatekeeper ===
    std::atomic<uint64_t> prerollToken_{ 0 };
    static std::atomic<uint64_t> nextUniquePrerollToken_;

    void releaseDecodeSlot(uint64_t tokenToRelease);
    void forceReleaseDecodeSlot();

    // === Tagged Sample Lock (Consumer/Producer Barrier) ===
    std::mutex sampleMutex_;
    TaggedSample stagedSample_;

    // === General Video Properties ===
    std::atomic<VideoDim> dimensions_{};
    std::atomic<int> playCount_{ 0 };
    std::atomic<int> numLoops_{ 0 };
    float volume_{ 0.0f };

    int allocatedWidth_{ 0 };
    int allocatedHeight_{ 0 };
    SDL_PixelFormatEnum allocatedFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    bool isTextureReady_{ false };
    int monitor_;
    bool softOverlay_;
    int perspectiveCorners_[8]{ 0 };
    bool hasPerspective_{ false };

    // === GStreamer and SDL resource pointers ===
    GstElement* pipeline_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* perspective_{ nullptr };
    int lastPerspectiveW_{ -1 };
    int lastPerspectiveH_{ -1 };
    SDL_Texture* texture_{ nullptr };
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    guint elementSetupHandlerId_{ 0 };
    guint busWatchId_{ 0 };
    guint padProbeId_{ 0 };
    GValueArray* perspective_gva_{ nullptr };

    static bool initialized_;
    static bool pluginsInitialized_;

    // Validation helper mapped to new state
    inline bool isCurrentEpoch(uint64_t e) const {
        const PipelineLifecycle lifecycle =
            lifecycle_.load(std::memory_order_acquire);
        return e == playbackEpoch_.load(std::memory_order_acquire) &&
            (lifecycle == PipelineLifecycle::Starting ||
                lifecycle == PipelineLifecycle::Ready);
    }

    static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstFlowReturn on_new_preroll(GstAppSink* sink, gpointer user_data);
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static GstFlowReturn on_audio_new_sample(GstAppSink* sink, gpointer user_data);
    static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static void initializePlugins();

    void createSdlTexture();
    void destroyTextures();
    bool updateTextureFromFrameIYUV(SDL_Texture*, GstVideoFrame*) const;
    bool updateTextureFromFrameNV12(SDL_Texture*, GstVideoFrame*) const;
    bool updateTextureFromFrameRGBA(SDL_Texture*, GstVideoFrame*) const;
    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;

    std::atomic<bool> loopsFinished_{ false };
    std::atomic<bool> hasVideoStream_{ true };

    // Tracks semantic discontinuity for audio crossfading
    std::atomic<uint64_t> lastFadedEpoch_{ 0 };

    // Audio bus integration. Source ID changes are serialized by the pipeline
    // mutex; the shared handle is atomically published to streaming callbacks.
    AudioBus::SourceId videoSourceId_{ 0 };
    std::shared_ptr<AudioBus::Handle> audioHandle_;
    GstElement* audioSink_{ nullptr };
};
