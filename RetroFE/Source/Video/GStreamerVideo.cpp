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
#include "GStreamerVideo.h"
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
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

bool GStreamerVideo::initialized_ = false;
bool GStreamerVideo::pluginsInitialized_ = false;

typedef enum
{
    GST_PLAY_FLAG_VIDEO = (1 << 0),
    GST_PLAY_FLAG_AUDIO = (1 << 1),
} GstPlayFlags;

static SDL_BlendMode softOverlayBlendMode = SDL_ComposeCustomBlendMode(
    SDL_BLENDFACTOR_SRC_ALPHA,           // Source color factor: modulates source color by the alpha value set dynamically
    SDL_BLENDFACTOR_ONE,                 // Destination color factor: keep the destination as is
    SDL_BLENDOPERATION_ADD,              // Color operation: add source and destination colors based on alpha
    SDL_BLENDFACTOR_ONE,                 // Source alpha factor
    SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, // Destination alpha factor: inverse of source alpha
    SDL_BLENDOPERATION_ADD               // Alpha operation: add alpha values
);

GStreamerVideo::GStreamerVideo(int monitor)

    : monitor_(monitor)

{
    initializePlugins();
}

GStreamerVideo::~GStreamerVideo() = default;

void GStreamerVideo::initializePlugins()
{
    if (!pluginsInitialized_)
    {
        pluginsInitialized_ = true;

#if defined(WIN32)
        //enablePlugin("directsoundsink");
        disablePlugin("mfdeviceprovider");
        if (!Configuration::HardwareVideoAccel)
        {
            //enablePlugin("openh264dec");
            disablePlugin("d3d11h264dec");
            disablePlugin("d3d11h265dec");
            disablePlugin("nvh264dec");
            enablePlugin("avdec_h264");
            enablePlugin("avdec_h265");
        }
        else
        {
            enablePlugin("d3d11h264dec");
            disablePlugin("nvh264dec");
            //disablePlugin("d3d11h264dec");

            //enablePlugin("qsvh264dec");
        }
#elif defined(__APPLE__)
        // if (Configuration::HardwareVideoAccel) {
        //     enablePlugin("vah264dec");
        //     enablePlugin("vah265dec");
        // }
#else
        enablePlugin("alsasink");
        disablePlugin("pulsesink");
        if (Configuration::HardwareVideoAccel)
        {
            enablePlugin("vah264dec");
            enablePlugin("vah265dec");
        }
        if (!Configuration::HardwareVideoAccel)
        {
            disablePlugin("vah264dec");
            disablePlugin("vah265dec");
            enablePlugin("openh264dec");
            disablePlugin("avdec_h264");
            disablePlugin("avdec_h265");
        }
#endif
    }
}

void GStreamerVideo::setNumLoops(int n)
{
    if (n > 0)
        numLoops_ = n;
}

SDL_Texture* GStreamerVideo::getTexture() const
{
    SDL_LockMutex(SDL::getMutex());
    SDL_Texture* texture = texture_;
    SDL_UnlockMutex(SDL::getMutex());
    return texture;
}

bool GStreamerVideo::initialize()
{
    if (initialized_)
    {
        initialized_ = true;
        return true;
    }
    if (!gst_is_initialized())
    {
        LOG_DEBUG("GStreamer", "Initializing in instance");
        gst_init(nullptr, nullptr);
        std::string path = Utils::combinePath(Configuration::absolutePath, "retrofe");
#ifdef WIN32
        GstRegistry *registry = gst_registry_get();
        gst_registry_scan_path(registry, path.c_str());
#endif
    }
    initialized_ = true;
    return true;
}

bool GStreamerVideo::deInitialize()
{
    gst_deinit();
    initialized_ = false;
    paused_ = false;
    return true;
}

bool GStreamerVideo::stop()
{
    if (!initialized_)
    {
        return false;
    }

    stopping_.store(true, std::memory_order_release);

    g_object_set(videoSink_, "signal-handoffs", FALSE, nullptr);

    newFrameAvailable_ = false;

    isPlaying_ = false;

    if (playbin_)
    {
        // Set the pipeline state to NULL
        gst_element_set_state(playbin_, GST_STATE_NULL);

        // Wait for the state change to complete
        GstState state;
        GstStateChangeReturn ret = gst_element_get_state(playbin_, &state, nullptr, GST_CLOCK_TIME_NONE);
        if (ret != GST_STATE_CHANGE_SUCCESS)
        {
            LOG_ERROR("Video", "Failed to change playbin state to NULL");
        }

        if (videoInfo_)
            gst_video_info_free(videoInfo_);
        
        // Clear the buffer queue
        bufferQueue_.clear();
        
        // Disconnect signal handlers
        if (elementSetupHandlerId_)
        {
            g_signal_handler_disconnect(playbin_, elementSetupHandlerId_);
            elementSetupHandlerId_ = 0;
        }
        if (aboutToFinishHandlerId_ != 0)
        {
            g_signal_handler_disconnect(playbin_, aboutToFinishHandlerId_);
            aboutToFinishHandlerId_ = 0;
        }

        if (handoffHandlerId_)
        {
            g_signal_handler_disconnect(videoSink_, handoffHandlerId_);
            handoffHandlerId_ = 0;
        }

        if (prerollHandlerId_)
        {
            g_signal_handler_disconnect(videoSink_, prerollHandlerId_);
            prerollHandlerId_ = 0;
        }

        gst_object_unref(playbin_);
        
        playbin_ = nullptr;
        videoSink_ = nullptr;
        capsFilter_ = nullptr;
        videoBin_ = nullptr;
        videoBus_ = nullptr;
        videoInfo_ = nullptr;



    }

    SDL_LockMutex(SDL::getMutex());
    if (texture_ != nullptr)
    {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    SDL_UnlockMutex(SDL::getMutex());

    return true;
}

bool GStreamerVideo::play(const std::string &file)
{
    playCount_ = 0;
    if (!initialized_)
        return false;

    currentFile_ = file;
    if (!initializeGstElements(file))
        return false;
    // Start playing
    if (GstStateChangeReturn playState = gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING);
        playState != GST_STATE_CHANGE_ASYNC)
    {
        isPlaying_ = false;
        LOG_ERROR("Video", "Unable to set the pipeline to the playing state.");
        stop();
        return false;
    }
    baseTime_ = gst_element_get_base_time(GST_ELEMENT(playbin_));
    paused_ = false;
    isPlaying_ = true;
    // Set the volume to zero and mute the video
    // gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_),
    // GST_STREAM_VOLUME_FORMAT_LINEAR, 0.0);
    // gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), true);

    if (Configuration::debugDotEnabled)
    {
        // Environment variable is set, proceed with dot file generation
        GstState state;
        GstState pending;
        // Wait up to 5 seconds for the state change to complete
        GstClockTime timeout = 5 * GST_SECOND; // Define your timeout
        GstStateChangeReturn ret = gst_element_get_state(GST_ELEMENT(playbin_), &state, &pending, timeout);
        if (ret == GST_STATE_CHANGE_SUCCESS && state == GST_STATE_PLAYING)
        {
            // The pipeline is in the playing state, proceed with dot file generation
            // Generate dot file for playbin_
            std::string playbinDotFileName = generateDotFileName("playbin", currentFile_);
            GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(playbin_), GST_DEBUG_GRAPH_SHOW_ALL, playbinDotFileName.c_str());
        }
    }
    return true;
}

bool GStreamerVideo::initializeGstElements(const std::string &file)
{
    gchar *uriFile = gst_filename_to_uri(file.c_str(), nullptr);
    gint flags;
    if (!uriFile)
    {
        LOG_DEBUG("Video", "Failed to convert filename to URI");
        return false;
    }

    playbin_ = gst_element_factory_make("playbin", "player");
    capsFilter_ = gst_element_factory_make("capsfilter", "caps_filter");
    videoBin_ = gst_bin_new("SinkBin");
    videoSink_ = gst_element_factory_make("fakesink", "video_sink");

    if (!playbin_ || !videoBin_ || !videoSink_ || !capsFilter_)
    {
        LOG_DEBUG("Video", "Could not create GStreamer elements");
        g_free(uriFile);
        return false;
    }

    g_object_get(playbin_, "flags", &flags, nullptr);
    flags |= GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
    g_object_set(playbin_, "flags", flags, nullptr);

    GstCaps *videoConvertCaps;
    if (Configuration::HardwareVideoAccel)
    {
        videoConvertCaps = gst_caps_from_string(
            "video/x-raw,format=(string)NV12,pixel-aspect-ratio=(fraction)1/1");
        sdlFormat_ = SDL_PIXELFORMAT_NV12;
        LOG_DEBUG("GStreamerVideo", "SDL pixel format selected: SDL_PIXELFORMAT_NV12. HarwareVideoAccel:true");
    }
    else
    {
        videoConvertCaps = gst_caps_from_string(
            "video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1");
        elementSetupHandlerId_ = g_signal_connect(playbin_, "element-setup", G_CALLBACK(elementSetupCallback), this);
        sdlFormat_ = SDL_PIXELFORMAT_IYUV;
        LOG_DEBUG("GStreamerVideo", "SDL pixel format selected: SDL_PIXELFORMAT_IYUV. HarwareVideoAccel:false");
    }

    g_object_set(capsFilter_, "caps", videoConvertCaps, nullptr);
    gst_caps_unref(videoConvertCaps);

    gst_bin_add_many(GST_BIN(videoBin_), capsFilter_, videoSink_, nullptr);
    if (!gst_element_link_many(capsFilter_, videoSink_, nullptr))
    {
        LOG_DEBUG("Video", "Could not link video processing elements");
        g_free(uriFile);
        return false;
    }

    GstPad *sinkPad = gst_element_get_static_pad(capsFilter_, "sink");
    GstPad *ghostPad = gst_ghost_pad_new("sink", sinkPad);
    gst_element_add_pad(videoBin_, ghostPad);
    gst_object_unref(sinkPad);

    g_object_set(playbin_, "uri", uriFile, "video-sink", videoBin_, nullptr);
    g_object_set(playbin_, "volume", 0.0, nullptr);

    g_free(uriFile);

    GstClock* clock = gst_system_clock_obtain();
    g_object_set(clock,
        "clock-type", GST_CLOCK_TYPE_MONOTONIC,
        nullptr);
    gst_pipeline_use_clock(GST_PIPELINE(playbin_), clock);
    gst_object_unref(clock);

    if (GstPad *pad = gst_element_get_static_pad(videoSink_, "sink"))
    {
        padProbeId_ = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, padProbeCallback, this, nullptr);
        gst_object_unref(pad);
    }

    videoBus_ = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
    gst_object_unref(videoBus_);

    g_object_set(videoSink_, "sync", TRUE, "enable-last-sample", FALSE, nullptr);
    bufferDisconnected_ = true;

    handoffHandlerId_ = g_signal_connect(videoSink_, "handoff", G_CALLBACK(processNewBuffer), this);

    return true;
}

void GStreamerVideo::elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data)
{
    // Check if the element is a video decoder
    if (GST_IS_VIDEO_DECODER(element))
    {
        if (!Configuration::HardwareVideoAccel && Utils::getOSType() != "linux")
        {
            // Configure the video decoder
            g_object_set(element, "thread-type", Configuration::AvdecThreadType,
                "max-threads", Configuration::AvdecMaxThreads,
                "direct-rendering", FALSE, nullptr);
        }
    }
}

GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    auto *video = static_cast<GStreamerVideo *>(user_data);

    auto *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS)
    {
        GstCaps *caps = nullptr;
        gst_event_parse_caps(event, &caps);
        if (caps)
        {
            if (gst_video_info_from_caps(video->videoInfo_, caps))
            {
                video->width_ = video->videoInfo_->width;
                video->height_ = video->videoInfo_->height;
                LOG_DEBUG("GStreamerVideo", "Video dimensions: width = " + std::to_string(video->width_) +
                                                ", height = " + std::to_string(video->height_));

                // Remove the pad probe after getting the video dimensions
                gst_pad_remove_probe(pad, video->padProbeId_);
                video->padProbeId_ = 0;
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

void GStreamerVideo::createSdlTexture()
{
    texture_ = SDL_CreateTexture(SDL::getRenderer(monitor_), sdlFormat_, SDL_TEXTUREACCESS_STREAMING, width_, height_);

    if (!texture_)
    {
        LOG_ERROR("GStreamerVideo", "SDL_CreateTexture failed: " + std::string(SDL_GetError()));
        return;
    }

    // Check if softOverlay_ is true and set the blend mode accordingly
    SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;

    if (SDL_SetTextureBlendMode(texture_, blendMode) != 0)
    {
        LOG_ERROR("GStreamerVideo", "SDL_SetTextureBlendMode failed: " + std::string(SDL_GetError()));
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
        return;
    }
}

void GStreamerVideo::loopHandler()
{
    if (videoBus_)
    {
        GstMessage *msg = gst_bus_pop_filtered(videoBus_, GST_MESSAGE_EOS);
        if (msg)
        {
            playCount_++;
            // If the number of loops is 0 or greater than the current playCount_,
            // seek the playback to the beginning.
            if (!numLoops_ || numLoops_ > playCount_)
            {
                gst_element_seek(playbin_, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
                                 GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
            }
            else
            {
                stop();
            }
            gst_message_unref(msg);
        }
    }
}

void GStreamerVideo::bufferDisconnect(bool disconnect)
{
    if (disconnect) {
        g_object_set(videoSink_, "signal-handoffs", FALSE, nullptr);
        bufferDisconnected_ = true;
    }
    else
    {
        g_object_set(videoSink_, "signal-handoffs", TRUE, nullptr);
        bufferDisconnected_ = false;
    }
}

bool GStreamerVideo::isBufferDisconnected()
{
    return bufferDisconnected_;
}

void GStreamerVideo::volumeUpdate()
{
    if (!isPlaying_)
        return;

    bool shouldMute = false;
    double targetVolume = 0.0;

    // Check if video should be muted based on configuration.
    if (bool muteVideo = Configuration::MuteVideo; muteVideo)
    {
        shouldMute = true;
    }
    else
    {
        // Ensure volume_ does not exceed the maximum value.
        if (volume_ > 1.0)
            volume_ = 1.0;

        // Adjust currentVolume_ towards the target volume_.
        if (currentVolume_ > volume_ || currentVolume_ + 0.005 >= volume_)
            currentVolume_ = volume_;
        else
            currentVolume_ += 0.005;

        // Set the target volume to the current calculated volume.
        targetVolume = currentVolume_;

        // Mute if the current volume is below a threshold.
        if (currentVolume_ < 0.1)
            shouldMute = true;
    }

    // Only set the volume if it has changed since the last call.
    if (targetVolume != lastSetVolume_)
    {
        g_object_set(playbin_, "volume", targetVolume, nullptr);
        lastSetVolume_ = targetVolume;
    }

    // Only set the mute state if it has changed since the last call.
    if (shouldMute != lastSetMuteState_)
    {
        // Use g_object_set to mute or unmute by setting the volume to 0.0 or the target volume.
        if (shouldMute)
        {
            g_object_set(playbin_, "volume", 0.0, nullptr);
        }
        else
        {
            g_object_set(playbin_, "volume", targetVolume, nullptr);
        }

        lastSetMuteState_ = shouldMute;
    }
}

int GStreamerVideo::getHeight()
{
    return height_;
}

int GStreamerVideo::getWidth()
{
    return width_;
}

GstClockTime GStreamerVideo::getLastPTS() const {
    return lastPTS_;
}

GstClockTime GStreamerVideo::getExpectedTime() const {
    return expectedTime_;
}

bool GStreamerVideo::isNewFrameAvailable() const {
    return newFrameAvailable_;
}

void GStreamerVideo::resetNewFrameFlag() {
    newFrameAvailable_ = false;
}

GstElement* GStreamerVideo::getPipeline() const {
    return playbin_;
}

GstElement* GStreamerVideo::getVideoSink() const {
    return videoSink_;
}

void GStreamerVideo::processNewBuffer(GstElement const* /* fakesink */, GstBuffer* buf, GstPad* new_pad, gpointer userdata) {
    auto* video = static_cast<GStreamerVideo*>(userdata);

    if (video->stopping_.load(std::memory_order_acquire)) {
        return;
    }

    gst_buffer_ref(buf);  // Copy the buffer for independent lifecycle management
    if (!video->bufferQueue_.push(buf)) {
        gst_buffer_unref(buf);  // Unref the buffer if it could not be pushed
    }
}

void GStreamerVideo::draw() {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }

    auto bufferOpt = bufferQueue_.pop();
    if (!bufferOpt.has_value()) {
        return;  // No buffer available
    }

    GstBuffer* buffer = *bufferOpt;

    if (!texture_ && width_ != 0 && height_ != 0) {
        createSdlTexture();  // Create the SDL texture if it doesn't exist
    }

    if (texture_) {
        GstVideoFrame vframe(GST_VIDEO_FRAME_INIT);
        if (gst_video_frame_map(&vframe, videoInfo_, buffer, (GstMapFlags)(GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF))) {
            SDL_LockMutex(SDL::getMutex());
            if (sdlFormat_ == SDL_PIXELFORMAT_NV12) {
                if (SDL_UpdateNVTexture(texture_, nullptr,
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0),
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1)) != 0) {
                    LOG_ERROR("GStreamerVideo", "Unable to update NV texture.");
                }
            }
            else if (sdlFormat_ == SDL_PIXELFORMAT_IYUV) {
                if (SDL_UpdateYUVTexture(texture_, nullptr,
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0),
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1),
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 2)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 2)) != 0) {
                    LOG_ERROR("GStreamerVideo", "Unable to update YUV texture.");
                }
            }
            SDL_UnlockMutex(SDL::getMutex());
            gst_video_frame_unmap(&vframe);  // Unmap the video frame after processing

            // Inform GStreamer about buffer processing
            GstClockTime pts = GST_BUFFER_PTS(buffer);
            if (!GST_CLOCK_TIME_IS_VALID(pts)) {
                LOG_DEBUG("GStreamerVideo", "Invalid PTS: Skipping QOS event for this buffer.");
                gst_buffer_unref(buffer);
                return;
            }

            // Store the PTS for later use
            lastPTS_ = pts;

            // Mark the new frame as available
            newFrameAvailable_ = true;
        }
    }
    gst_buffer_unref(buffer);  // Release the buffer after use
}

bool GStreamerVideo::isPlaying()
{
    return isPlaying_;
}

void GStreamerVideo::setVolume(float volume)
{
    if (!isPlaying_)
        return;
    volume_ = volume;
}

void GStreamerVideo::skipForward()
{
    if (!isPlaying_)
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

void GStreamerVideo::skipBackward()
{
    if (!isPlaying_)
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

void GStreamerVideo::skipForwardp()
{
    if (!isPlaying_)
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

void GStreamerVideo::skipBackwardp()
{

    if (!isPlaying_)
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

void GStreamerVideo::pause()
{
    if (!isPlaying_)
        return;

    paused_ = !paused_;

    if (paused_)
    {
        g_object_set(videoSink_, "signal-handoffs", FALSE, nullptr);
        gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PAUSED);
    }
    else
    {
        g_object_set(videoSink_, "signal-handoffs", TRUE, nullptr);
        gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING);
    }
}

void GStreamerVideo::restart()
{
    if (!isPlaying_)
        return;

    gst_element_seek_simple(playbin_, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH), 0);
}

unsigned long long GStreamerVideo::getCurrent()
{
    gint64 ret = 0;
    if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &ret) || !isPlaying_)
        ret = 0;
    return (unsigned long long)ret;
}

unsigned long long GStreamerVideo::getDuration()
{
    gint64 ret = 0;
    if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &ret) || !isPlaying_)
        ret = 0;
    return (unsigned long long)ret;
}

bool GStreamerVideo::isPaused()
{
    return paused_;
}

std::string GStreamerVideo::generateDotFileName(const std::string &prefix, const std::string &videoFilePath) const
{
    std::string videoFileName = Utils::getFileName(videoFilePath);

    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

    std::stringstream ss;
    ss << prefix << "_" << videoFileName << "_" << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S_")
       << std::setfill('0') << std::setw(6) << microseconds.count();

    return ss.str();
}

void GStreamerVideo::enablePlugin(const std::string &pluginName)
{
    GstElementFactory *factory = gst_element_factory_find(pluginName.c_str());
    if (factory)
    {
        // Sets the plugin rank to PRIMARY + 1 to prioritize its use
        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 1);
        gst_object_unref(factory);
    }
}

void GStreamerVideo::disablePlugin(const std::string &pluginName)
{
    GstElementFactory *factory = gst_element_factory_find(pluginName.c_str());
    if (factory)
    {
        // Sets the plugin rank to GST_RANK_NONE to disable its use
        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
        gst_object_unref(factory);
    }
}

void GStreamerVideo::setSoftOverlay(bool value)
{
    softOverlay_ = value;
}