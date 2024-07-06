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
#include <atomic>
#include <mutex>
#include <queue>
#include <shared_mutex>

extern "C"
{
#if (__APPLE__)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/video.h>
#else
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>

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
    static void enablePlugin(const std::string &pluginName);
    static void disablePlugin(const std::string &pluginName);

  private:
    static void processNewBuffer(GstElement const * /* fakesink */, const GstBuffer *buf, GstPad *new_pad,
                                 gpointer userdata);
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static void sourceSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstPadProbeReturn padProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
    static void initializePlugins();
    bool initializeGstElements(const std::string &file);
    void createSdlTexture();
    GstElement *playbin_{nullptr};
    GstElement *videoSink_{nullptr};
    GstElement* videoBin_{ nullptr };
    GstElement* capsFilter_{ nullptr };
    GstBus *videoBus_{nullptr};
    GstVideoInfo* videoInfo_{ gst_video_info_new() };
    SDL_Texture *texture_{nullptr};
    SDL_PixelFormatEnum sdlFormat_{SDL_PIXELFORMAT_UNKNOWN};
    guint elementSetupHandlerId_{0};
    guint sourceSetupHandlerId_{ 0 };
    guint handoffHandlerId_{0};
    guint padProbeId_{0};
    guint prerollHandlerId_{0};
    gint height_{0};
    gint width_{0};
    GAsyncQueue *bufferQueue_;
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
    std::atomic<bool> stopping_{false};
    std::shared_mutex stopMutex_;
    static bool pluginsInitialized_;

    std::string generateDotFileName(const std::string &prefix, const std::string &videoFilePath) const;
};
