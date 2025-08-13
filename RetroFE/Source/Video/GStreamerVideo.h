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

#include "IVideo.h"
#include "../SDL.h"
#include <string>
#include <array>
#include <atomic>
#include <functional>

extern "C" {
#if (__APPLE__)
#if __has_include(<gstreamer-1.0/gst/gst.h>)
#include <gstreamer-1.0/gst/gst.h>
#include <gstreamer-1.0/gst/video/video.h>
#elif __has_include(<GStreamer/gst/gst.h>)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/video.h>
#else
#error "Cannot find Gstreamer headers"
#endif
#else
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#endif
}

class GStreamerVideo final : public IVideo {

public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo&) = delete;
    GStreamerVideo& operator=(const GStreamerVideo&) = delete;
    ~GStreamerVideo() override;

    // --- Interface methods ---
    bool initialize() override;
    bool deInitialize() override;
    bool unload() override;
    bool play(const std::string& file) override;
    bool stop() override;
    SDL_Texture* getTexture() const override;
    void volumeUpdate() override;
    void draw() override;
    void setNumLoops(int n);
    int getHeight() override;
    int getWidth() override;
    bool isPlaying() override;
    void setVolume(float volume) override;
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

    bool hasError() const override {
        return hasError_.load(std::memory_order_acquire);
    }
    IVideo::VideoState getTargetState() const override { return targetState_; }
    IVideo::VideoState getActualState() const override {
        return actualState_.load(std::memory_order_acquire);
    }
    void setActualState(IVideo::VideoState s) {
        actualState_.store(s, std::memory_order_release);
    }
    bool isPipelineReady() const override {
        return pipeLineReady_;
    }


private:
    // === Thread-shared atomics ===
    std::atomic<uint64_t> currentPlaySessionId_{ 0 };
    std::atomic<IVideo::VideoState> actualState_{ IVideo::VideoState::None };
    static std::atomic<uint64_t> nextUniquePlaySessionId_;
    std::atomic<bool> hasError_{ false };              // Set by pad probe, read main
    std::atomic<uint64_t> lastMatrixSessionId_{ 0 };
    int lastMatW_ = 0, lastMatH_ = 0;

    // === Main-thread only ===
    IVideo::VideoState targetState_{ IVideo::VideoState::None };

    int width_{ -1 };
    int height_{ -1 };
    int playCount_{ 0 };
    std::string currentFile_{};
    int numLoops_{ 0 };
    float volume_{ 0.0f };
    double currentVolume_{ 0.0 };
    int monitor_;
    double lastSetVolume_{ -1.0 };
    bool lastSetMuteState_{ false };
    bool softOverlay_;
    int perspectiveCorners_[8]{ 0 };
    bool hasPerspective_{ false };
    bool pipeLineReady_{ false };

    // === GStreamer and SDL resource pointers ===
    GstElement* playbin_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* perspective_{ nullptr };
    SDL_Texture* videoTexture_{ nullptr };
    SDL_Texture* texture_{ nullptr };
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    guint elementSetupHandlerId_{ 0 };
    guint busWatchId_{ 0 };
    GValueArray* perspective_gva_{ nullptr };
    std::function<bool(SDL_Texture*, GstVideoFrame*)> updateTextureFunc_;

    std::atomic<GstSample*> stagedSample_{ nullptr };

    // === Static/shared ===
    static bool initialized_;
    static bool pluginsInitialized_;

    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);


    static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstFlowReturn on_new_preroll(GstAppSink* sink, gpointer user_data);
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static void initializePlugins();
    static GstPadProbeReturn caps_probe(GstPad* pad, GstPadProbeInfo* info, gpointer ud);
    bool createPipelineIfNeeded();
    void createSdlTexture();
    void initializeUpdateFunction();
    bool updateTextureFromFrameIYUV(SDL_Texture*, GstVideoFrame*) const;
    bool updateTextureFromFrameNV12(SDL_Texture*, GstVideoFrame*) const;
    bool updateTextureFromFrameRGBA(SDL_Texture*, GstVideoFrame*) const;
    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;
};

