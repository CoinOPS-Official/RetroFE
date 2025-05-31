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
#include "IVideo.h"
#include <atomic>
#include <string>
#include <functional>
#include <mutex>
#include <vector>

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
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#endif
}

class GStreamerVideo final : public IVideo {

    std::function<void(int width, int height)> dimensionsReadyCallback_;

public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo&) = delete;
    GStreamerVideo& operator=(const GStreamerVideo&) = delete;
    ~GStreamerVideo() override;

    // --- Interface methods ---
    bool initialize() override;
    bool deInitialize() override;
    bool unload();
    bool createPipelineIfNeeded();
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
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;
    void setSoftOverlay(bool value);
    void setPerspectiveCorners(const int* corners);

    bool hasError() const override {
        return hasError_.load(std::memory_order_acquire);
    }
    IVideo::VideoState getTargetState() const override { return targetState_; }
    IVideo::VideoState getActualState() const override { return actualState_; }
    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);

    void setDimensionsReadyCallback(std::function<void(int, int)> cb) {
        dimensionsReadyCallback_ = std::move(cb);
    }

private:
    // === Thread-shared atomics ===
    std::atomic<uint64_t> currentPlaySessionId_{ 0 };
    static std::atomic<uint64_t> nextUniquePlaySessionId_;
    std::atomic<bool> hasError_{ false };              // Set by pad probe, read main

    // === Main-thread only ===
    IVideo::VideoState targetState_{ IVideo::VideoState::None };
	IVideo::VideoState actualState_{ IVideo::VideoState::None };

    bool isPlaying_{false};
    uint64_t mappingGeneration_{ 0 };
    int width_{ 0 };
    int height_{ 0 };
    int textureWidth_{ -1 };
    int textureHeight_{ -1 };
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

    // === GStreamer and SDL resource pointers ===
    GstElement* playbin_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* perspective_{ nullptr };
    SDL_Texture* videoTexture_{ nullptr };
    SDL_Texture* alphaTexture_{ nullptr };
    SDL_Texture* texture_{ nullptr };
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    guint elementSetupHandlerId_{ 0 };
    guint padProbeId_{ 0 };
    guint busWatchId_{ 0 };
    GValueArray* gva_{ nullptr };
    GValueArray* perspective_gva_{ nullptr };
    std::function<bool(SDL_Texture*, GstVideoFrame*)> updateTextureFunc_;

    // === Static/shared ===
    static constexpr int ALPHA_TEXTURE_SIZE = 4;
    static std::vector<GStreamerVideo*> activeVideos_;
    static std::mutex activeVideosMutex_;
    static bool initialized_;
    static bool pluginsInitialized_;

    mutable std::mutex drawMutex_;

    // === Internal helpers ===
    struct PadProbeUserdata {
        GStreamerVideo* videoInstance;
        uint64_t playSessionId;
    };
    static GStreamerVideo* findInstanceFromGstObject(GstObject* object);
    void createAlphaTexture();
    static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static void initializePlugins();
    void createSdlTexture();
    void initializeUpdateFunction();
    bool updateTextureFromFrameIYUV(SDL_Texture*, GstVideoFrame*) const;
    bool updateTextureFromFrameNV12(SDL_Texture*, GstVideoFrame*) const;
    bool updateTextureFromFrameRGBA(SDL_Texture*, GstVideoFrame*) const;
    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;
    static void customGstLogHandler(GstDebugCategory* category, GstDebugLevel level, const gchar* file, const gchar* function, gint line, GObject* object, GstDebugMessage* message, gpointer user_data);
};

