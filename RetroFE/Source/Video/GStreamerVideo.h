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
#include "IVideo.h"
extern "C"
{
#if (__APPLE__)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/gstvideometa.h>
#else
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>

#endif
}

class GStreamerVideo final : public IVideo
{
  public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo &) = delete;
    GStreamerVideo &operator=(const GStreamerVideo &) = delete;
    ~GStreamerVideo() override;
    bool initialize() override;
    bool play(const std::string &file) override;
    bool stop() override;
    bool deInitialize() override;
    SDL_Texture *getTexture() const override;
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    void update(float dt) override;
    void loopHandler() override;
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
    // Helper functions...
    static void enablePlugin(const std::string &pluginName);
    static void disablePlugin(const std::string &pluginName);

  private:
    static void elementSetupCallback([[maybe_unused]] GstElement const *playbin, GstElement *element,
                                     [[maybe_unused]] GStreamerVideo const *video);
    bool initializeGstElements(const std::string &file);
    GstElement *playbin_{nullptr};
    GstElement *videoSink_{nullptr};
    GstBus *videoBus_{nullptr};
    GstVideoInfo videoInfo_;
    SDL_Texture *texture_{nullptr};
    guint elementSetupHandlerId_{0};
    guint handoffHandlerId_{0};
    guint padProbeId_{0};
    gint height_{0};
    gint width_{0};
    bool isPlaying_{false};
    static bool initialized_;
    int playCount_{0};
    std::string currentFile_{};
    int numLoops_{0};
    float volume_{0.0f};
    double currentVolume_{0.0};
    int monitor_;
    bool paused_{false};
    double lastSetVolume_{0.0};
    bool lastSetMuteState_{false};

    std::string generateDotFileName(const std::string &prefix, const std::string &videoFilePath);
};
