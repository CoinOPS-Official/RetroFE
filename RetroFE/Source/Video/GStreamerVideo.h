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
#include <shared_mutex>
#include <string>
#include <optional>


extern "C" {
#if (__APPLE__)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/video.h>
#else
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#endif
}



class GStreamerVideo final : public IVideo {
public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo&) = delete;
    GStreamerVideo& operator=(const GStreamerVideo&) = delete;
    ~GStreamerVideo() override;
    void messageHandler() override;
    bool initialize() override;
    bool unload();
    bool createPipelineIfNeeded();
    bool play(const std::string& file) override;
    bool stop() override;
    bool deInitialize() override;
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
    void restart() override;
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;
    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);

    void setSoftOverlay(bool value);

    void setPerspectiveCorners(const int* corners);

    bool hasError() const override {
        return hasError_.load(std::memory_order_acquire);
    }

private:
    static constexpr int ALPHA_TEXTURE_SIZE = 4;
    void createAlphaTexture();
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static void initializePlugins();
    void createSdlTexture();
    GstElement* playbin_{ nullptr };          // for playbin3
    GstElement* videoSink_{ nullptr };     // for appsink
    GstElement* perspective_{ nullptr };
    GstVideoInfo* videoInfo_{ nullptr };
    SDL_Texture* videoTexture_ = nullptr;    // YUV texture for video content
    SDL_Texture* alphaTexture_ = nullptr;    // Transparent texture for transitions
    SDL_Texture* texture_ = nullptr;         // Points to either videoTexture_ or alphaTexture_
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    guint elementSetupHandlerId_{ 0 };
    guint padProbeId_{ 0 };
    GValueArray* gva_;
    std::atomic<int> width_{ 0 };
    std::atomic<int> height_{ 0 };
	int textureWidth_{ -1 };
	int textureHeight_{ -1 };
	bool textureValid_{ false };
    std::atomic<bool> isPlaying_{ false };
    static bool initialized_;
    int playCount_{ 0 };
    std::string currentFile_{};
    int numLoops_{ 0 };
    float volume_{ 0.0f };
    double currentVolume_{ 0.0 };
    int monitor_;
    bool paused_{ false };
    double lastSetVolume_{ -1.0 };
    bool lastSetMuteState_{ false };
    std::atomic<bool> stopping_{ false };
    static bool pluginsInitialized_;
    bool softOverlay_;
    int counter_; // Counter for animation

    std::atomic<bool> hasError_{false};

    int perspectiveCorners_[8]{ 0 };
    bool hasPerspective_{ false };

    GValueArray* perspective_gva_{ nullptr };

    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;
};