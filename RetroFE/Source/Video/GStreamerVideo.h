#pragma once

#include "../Database/Configuration.h"
#include "../Graphics/SingletonThreadPool.h"
#include "../SDL.h"
#include "../Utility/Utils.h"
#include "IVideo.h"
#include <atomic>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

extern "C"
{
#if (__APPLE__)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/video.h>
#else
#include <gst/gst.h>
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

    // IVideo interface methods
    bool initialize() override;
    bool play(const std::string &file) override;
    bool stop() override;
    bool deInitialize() override;
    SDL_Texture *getTexture() const override;
    void loopHandler() override;
    void volumeUpdate() override;
    void update(float dt) override;
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

  private:
    // GStreamer callback methods
    static void processNewBuffer(GstElement const *fakesink, GstBuffer *buf, GstPad *new_pad, gpointer userdata);
    static void elementSetupCallback(const GstElement &playbin, GstElement *element, GStreamerVideo *video);
    static GstPadProbeReturn padProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);

    // GStreamer utility methods
    static void enablePlugin(const std::string &pluginName);
    static void disablePlugin(const std::string &pluginName);
    bool initializeGstElements(const std::string &file);
    void createSdlTexture();
    void clearBuffers();

    // GStreamer elements
    GstElement *playbin_ = nullptr;
    GstElement *videoSink_ = nullptr;
    GstBus *videoBus_ = nullptr;
    GstVideoInfo videoInfo_;

    // SDL elements
    SDL_Texture *texture_ = nullptr;
    SDL_PixelFormatEnum sdlFormat_ = SDL_PIXELFORMAT_UNKNOWN;

    // GStreamer handler IDs
    guint elementSetupHandlerId_ = 0;
    guint handoffHandlerId_ = 0;
    guint padProbeId_ = 0;

    // Video properties
    gint height_ = 0;
    gint width_ = 0;

    // Atomic flags
    std::atomic<bool> isPlaying_ = false;
    static std::atomic<bool> initialized_;
    static std::once_flag initFlag_;
    std::atomic<bool> paused_ = false;
    std::atomic<bool> frameReady_ = false;
    std::atomic<bool> bufferQueueEmpty_ = true;
    std::atomic<bool> stopping_ = false;

    // Synchronization
    std::mutex syncMutex_;
    std::queue<GstBuffer *> bufferQueue_;
    std::future<void> stopFuture_;

    // Other properties
    int playCount_ = 0;
    std::string currentFile_;
    int numLoops_ = 0;
    float volume_ = 0.0f;
    double currentVolume_ = 0.0;
    int monitor_;
    double lastSetVolume_ = 0.0;
    bool lastSetMuteState_ = false;

    // Utility methods
    std::string generateDotFileName(const std::string &prefix, const std::string &videoFilePath) const;
};
