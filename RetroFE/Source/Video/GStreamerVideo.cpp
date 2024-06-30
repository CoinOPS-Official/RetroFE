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
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/gstdebugutils.h>
#include <gst/video/video.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

bool GStreamerVideo::initialized_ = false;

GStreamerVideo::GStreamerVideo(int monitor)

    : monitor_(monitor)

{
    gst_video_info_init(&videoInfo_);
    bufferQueue_ = g_async_queue_new();
}

GStreamerVideo::~GStreamerVideo()
{
    GStreamerVideo::stop();
}

void GStreamerVideo::async_set_state_null(GstElement *element, gpointer user_data)
{
    gst_element_set_state(element, GST_STATE_NULL);

    // Optionally perform a quick, non-blocking state check
    GstStateChangeReturn ret = gst_element_get_state(element, nullptr, nullptr, 0);
    if (ret != GST_STATE_CHANGE_SUCCESS && ret != GST_STATE_CHANGE_ASYNC)
    {
        LOG_ERROR("Video", "Unexpected state change result when stopping playback");
    }

    gst_object_unref(GST_OBJECT(element));
}

void GStreamerVideo::async_set_state_paused(GstElement *element, gpointer user_data)
{
    auto *video = static_cast<GStreamerVideo *>(user_data);

    if (video->videoSink_ && video->handoffHandlerId_ != 0 && video->prerollHandlerId_ != 0)
    {
        g_signal_handler_disconnect(video->videoSink_, video->handoffHandlerId_);
        g_signal_handler_disconnect(video->videoSink_, video->prerollHandlerId_);
        video->handoffHandlerId_ = 0;
        video->prerollHandlerId_ = 0;
    }

    gst_element_set_state(element, GST_STATE_PAUSED);
}

void GStreamerVideo::async_set_state_playing(GstElement *element, gpointer user_data)
{
    auto *video = static_cast<GStreamerVideo *>(user_data);

    if (video->videoSink_ && video->handoffHandlerId_ == 0 && video->prerollHandlerId_ == 0)
    {
        video->prerollHandlerId_ =
            g_signal_connect(video->videoSink_, "preroll-handoff", G_CALLBACK(processNewBuffer), video);
        video->handoffHandlerId_ = g_signal_connect(video->videoSink_, "handoff", G_CALLBACK(processNewBuffer), video);
    }

    gst_element_set_state(element, GST_STATE_PLAYING);
}

void GStreamerVideo::async_seek_to_start(GstElement *element, gpointer user_data)
{
    gst_element_seek_simple(element, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH), 0);
}

void GStreamerVideo::setNumLoops(int n)
{
    if (n > 0)
        numLoops_ = n;
}

SDL_Texture *GStreamerVideo::getTexture() const
{
    return texture_;
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

    isPlaying_ = false;

    if (videoSink_ && handoffHandlerId_ != 0)
    {
        g_signal_handler_disconnect(videoSink_, handoffHandlerId_);
        handoffHandlerId_ = 0;
    }

    if (videoSink_ && prerollHandlerId_ != 0)
    {
        g_signal_handler_disconnect(videoSink_, prerollHandlerId_);
        prerollHandlerId_ = 0;
    }

    // Initiate the transition of playbin to GST_STATE_NULL without waiting
    if (playbin_) {
        gst_element_set_state(playbin_, GST_STATE_NULL);

        // Optionally perform a quick, non-blocking state check
        GstStateChangeReturn ret = gst_element_get_state(playbin_, nullptr, nullptr, 0);
        if (ret != GST_STATE_CHANGE_SUCCESS && ret != GST_STATE_CHANGE_ASYNC) {
            LOG_ERROR("Video", "Unexpected state change result when stopping playback");
        }
        g_object_unref(playbin_);
    }

    if (bufferQueue_)
    {
        while (g_async_queue_length(bufferQueue_) > 0)
        {
            auto bufferFrame = static_cast<BufferFrame*>(g_async_queue_pop(bufferQueue_));
            if (bufferFrame)
            {
                BufferFramePtr framePtr(bufferFrame);  // This will automatically unmap and delete the frame and buffer.
            }
        }

        g_async_queue_unref(bufferQueue_);
        bufferQueue_ = nullptr;
    }


    // Release SDL Texture
    if (texture_)
    {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    // Reset remaining pointers and variables to ensure the object is in a clean
    // state.
    playbin_ = nullptr;
    videoSink_ = nullptr;
    videoBus_ = nullptr;

    return true;
}

bool GStreamerVideo::play(const std::string &file)
{
    playCount_ = 0;
    if (!initialized_)
        return false;
#if defined(WIN32)
    enablePlugin("directsoundsink");
    disablePlugin("mfdeviceprovider");
    if (!Configuration::HardwareVideoAccel)
    {
        //enablePlugin("openh264dec");
        disablePlugin("d3d11h264dec");
        disablePlugin("d3d11h265dec");
        enablePlugin("avdec_h264");
        enablePlugin("avdec_h265");
    }
    else
    {
        // disablePlugin("d3d11h264dec");

        // enablePlugin("qsvh264dec");
    }
#elif defined(__APPLE__)
        // if (Configuration::HardwareVideoAccel) {
        //     enablePlugin("vah264dec");
        //     enablePlugin("vah265dec");
        // }
#else
    if (Configuration::HardwareVideoAccel)
    {
        enablePlugin("vah264dec");
        enablePlugin("vah265dec");
    }
    if (!Configuration::HardwareVideoAccel)
    {
        disablePlugin("vah264dec");
        disablePlugin("vah265dec");
        enablePlugin("avdec_h264");
        enablePlugin("avdec_h265");
    }
#endif
    currentFile_ = file;
    if (!initializeGstElements(file))
        return false;
    // Start playing
    // Start playing
    if (GstStateChangeReturn playState = gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING); playState != GST_STATE_CHANGE_ASYNC) {
        isPlaying_ = false;
        LOG_ERROR("Video", "Unable to set the pipeline to the playing state.");
        stop();
        return false;
    }
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
    if (!uriFile)
    {
        LOG_DEBUG("Video", "Failed to convert filename to URI");
        return false;
    }

    playbin_ = gst_element_factory_make("playbin", "player");
    GstElement *capsFilter = gst_element_factory_make("capsfilter", "caps_filter");
    GstElement *videoBin = gst_bin_new("SinkBin");
    videoSink_ = gst_element_factory_make("fakesink", "video_sink");

    if (!playbin_ || !videoBin || !videoSink_ || !capsFilter)
    {
        LOG_DEBUG("Video", "Could not create GStreamer elements");
        g_free(uriFile);
        return false;
    }

    GstCaps *videoConvertCaps = nullptr;
    if (Configuration::HardwareVideoAccel)
    {
        videoConvertCaps = gst_caps_from_string(
            "video/x-raw(memory:D3D11Memory),format=(string)NV12,pixel-aspect-ratio=(fraction)1/1");
        sdlFormat_ = SDL_PIXELFORMAT_NV12;
        LOG_DEBUG("GStreamerVideo", "SDL pixel format selected: SDL_PIXELFORMAT_NV12. HarwareVideoAccel:true");
    }
    else
    {
        videoConvertCaps = gst_caps_from_string("video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1");
        sdlFormat_ = SDL_PIXELFORMAT_IYUV;
        LOG_DEBUG("GStreamerVideo", "SDL pixel format selected: SDL_PIXELFORMAT_IYUV. HarwareVideoAccel:false");
    }

    g_object_set(capsFilter, "caps", videoConvertCaps, nullptr);
    gst_caps_unref(videoConvertCaps);


    gst_bin_add_many(GST_BIN(videoBin),  capsFilter, videoSink_, nullptr);
    if (!gst_element_link_many( capsFilter, videoSink_, nullptr))
    {
        LOG_DEBUG("Video", "Could not link video processing elements");
        g_free(uriFile);
        return false;
    }

    GstPad *sinkPad = gst_element_get_static_pad(capsFilter, "sink");
    GstPad *ghostPad = gst_ghost_pad_new("sink", sinkPad);
    gst_element_add_pad(videoBin, ghostPad);
    gst_object_unref(sinkPad);

    const guint PLAYBIN_FLAGS = 0x00000001 | 0x00000002;
    g_object_set(playbin_, "uri", uriFile, "video-sink", videoBin, "instant-uri", TRUE, "flags", PLAYBIN_FLAGS,
                 nullptr);
    g_object_set(playbin_, "volume", 0.0, nullptr);

    g_free(uriFile);

    // Add pad probe directly in initializeGstElements()
    GstPad *pad = gst_element_get_static_pad(videoSink_, "sink");
    if (pad)
    {
        padProbeId_ = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, padProbeCallback, this, nullptr);
        gst_object_unref(pad);
    }

    elementSetupHandlerId_ = g_signal_connect(playbin_, "element-setup", G_CALLBACK(elementSetupCallback), nullptr);
    videoBus_ = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
    gst_object_unref(videoBus_);

    g_object_set(videoSink_, "signal-handoffs", TRUE, "sync", TRUE, "enable-last-sample", FALSE, nullptr);

    handoffHandlerId_ = g_signal_connect(videoSink_, "handoff", G_CALLBACK(processNewBuffer), this);
    prerollHandlerId_ = g_signal_connect(videoSink_, "preroll-handoff", G_CALLBACK(processNewBuffer), this);

    return true;
}

void GStreamerVideo::elementSetupCallback([[maybe_unused]] const GstElement &playbin, GstElement *element,
                                          [[maybe_unused]] GStreamerVideo *video)
{
    gchar *elementName = gst_element_get_name(element);

    if (!Configuration::HardwareVideoAccel)
    {
        if (g_str_has_prefix(elementName, "avdec_h26"))
        {
            // Modify the properties of the avdec_h26x element here
            g_object_set(element, "thread-type", Configuration::AvdecThreadType, "max-threads",
                         Configuration::AvdecMaxThreads, "direct-rendering", FALSE, nullptr);
        }
    }
#ifdef WIN32
    if (g_str_has_prefix(elementName, "wasapi"))
    {
        g_object_set(element, "low-latency", TRUE, nullptr);
    }
#endif

    g_free(elementName);
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
            if (gst_video_info_from_caps(&video->videoInfo_, caps))
            {
                video->width_ = video->videoInfo_.width;
                video->height_ = video->videoInfo_.height;
                LOG_DEBUG("GStreamerVideo", "Video dimensions: width = " + std::to_string(video->width_) +
                                                ", height = " + std::to_string(video->height_));

                // Remove the pad probe after getting the video dimensions
                gst_pad_remove_probe(pad, video->padProbeId_);
            }
            gst_caps_unref(caps);
        }
    }
    return GST_PAD_PROBE_OK;
}
void GStreamerVideo::createSdlTexture()
{
    LOG_DEBUG("GStreamerVideo", "Creating SDL texture with width: " + std::to_string(width_) +
                                    ", height: " + std::to_string(height_) + ", format: " + std::to_string(sdlFormat_));

    SDL_LockMutex(SDL::getMutex());

    texture_ = SDL_CreateTexture(SDL::getRenderer(monitor_), sdlFormat_, SDL_TEXTUREACCESS_STREAMING, width_, height_);

    if (!texture_)
    {
        LOG_ERROR("GStreamerVideo", "SDL_CreateTexture failed: " + std::string(SDL_GetError()));
        return;
    }

    if (SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND) != 0)
    {
        LOG_ERROR("GStreamerVideo", "SDL_SetTextureBlendMode failed: " + std::string(SDL_GetError()));
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
        return;
    }
    SDL_UnlockMutex(SDL::getMutex());

    LOG_DEBUG("GStreamerVideo", "SDL texture created and blend mode set successfully. Texture pointer: " +
                                    std::to_string(reinterpret_cast<std::uintptr_t>(texture_)));
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

void GStreamerVideo::volumeUpdate()
{
    if (!isPlaying_)
        return;
    bool shouldMute = false;
    double targetVolume = 0.0;
    if (bool muteVideo = Configuration::MuteVideo; muteVideo)
    {
        shouldMute = true;
    }
    else
    {
        if (volume_ > 1.0)
            volume_ = 1.0;
        if (currentVolume_ > volume_ || currentVolume_ + 0.005 >= volume_)
            currentVolume_ = volume_;
        else
            currentVolume_ += 0.005;
        targetVolume = currentVolume_;
        if (currentVolume_ < 0.1)
            shouldMute = true;
    }
    // Only set the volume if it has changed since the last call.
    if (targetVolume != lastSetVolume_)
    {
        gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_), GST_STREAM_VOLUME_FORMAT_LINEAR, targetVolume);
        lastSetVolume_ = targetVolume;
    }
    // Only set the mute state if it has changed since the last call.
    if (shouldMute != lastSetMuteState_)
    {
        gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), shouldMute);
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

void GStreamerVideo::processNewBuffer(GstElement const* /* fakesink */, const GstBuffer* buf, GstPad* new_pad, gpointer userdata)
{
    auto* video = static_cast<GStreamerVideo*>(userdata);
    if (video && !video->stopping_.load(std::memory_order_acquire))
    {
        if (g_async_queue_length(video->bufferQueue_) >= 20)
        {
            auto oldFrame = static_cast<BufferFrame*>(g_async_queue_pop(video->bufferQueue_));
            if (oldFrame)
            {
                BufferFramePtr framePtr(oldFrame);
                LOG_DEBUG("Video", "Buffer queue limit reached. Oldest buffer removed.");
            }
        }

        GstBuffer* copied_buf = gst_buffer_copy(buf);
        auto vframe = std::make_unique<GstVideoFrame>();
        auto map_flags = static_cast<GstMapFlags>(GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF);
        if (gst_video_frame_map(vframe.get(), &video->videoInfo_, copied_buf, map_flags))
        {
            BufferFramePtr bufferFramePtr(new BufferFrame(copied_buf, vframe.release()));
            g_async_queue_push(video->bufferQueue_, bufferFramePtr.release());
            int queue_size = g_async_queue_length(video->bufferQueue_);
            LOG_DEBUG("Video", "Buffer received, copied, mapped, and added to queue. "
                "Current queue size: " + std::to_string(queue_size));
        }
        else
        {
            gst_clear_buffer(&copied_buf);
        }
    }
}

void GStreamerVideo::update(float /* dt */)
{
    if (stopping_.load(std::memory_order_acquire) || !isPlaying_)
    {
        return;
    }

    auto bufferFrame = static_cast<BufferFrame*>(g_async_queue_try_pop(bufferQueue_));
    if (!bufferFrame)
    {
        return;
    }

    BufferFramePtr framePtr(bufferFrame);  // Use smart pointer for automatic cleanup

    if (!texture_ && width_ != 0 && height_ != 0)
    {
        createSdlTexture();
    }

    if (texture_ && framePtr->vframe)
    {
        SDL_LockMutex(SDL::getMutex());

        if (sdlFormat_ == SDL_PIXELFORMAT_NV12)
        {
            if (SDL_UpdateNVTexture(texture_, nullptr,
                static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(framePtr->vframe, 0)),
                GST_VIDEO_FRAME_PLANE_STRIDE(framePtr->vframe, 0),
                static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(framePtr->vframe, 1)),
                GST_VIDEO_FRAME_PLANE_STRIDE(framePtr->vframe, 1)) != 0)
            {
                LOG_ERROR("Video", "SDL_UpdateNVTexture failed: " + std::string(SDL_GetError()));
            }
        }
        else if (sdlFormat_ == SDL_PIXELFORMAT_IYUV)
        {
            if (SDL_UpdateYUVTexture(texture_, nullptr,
                static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(framePtr->vframe, 0)),
                GST_VIDEO_FRAME_PLANE_STRIDE(framePtr->vframe, 0),
                static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(framePtr->vframe, 1)),
                GST_VIDEO_FRAME_PLANE_STRIDE(framePtr->vframe, 1),
                static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(framePtr->vframe, 2)),
                GST_VIDEO_FRAME_PLANE_STRIDE(framePtr->vframe, 2)) != 0)
            {
                LOG_ERROR("Video", "SDL_UpdateYUVTexture failed: " + std::string(SDL_GetError()));
            }
        }
        else
        {
            LOG_ERROR("Video", "Unsupported format or fallback handling required.");
        }

        SDL_UnlockMutex(SDL::getMutex());
    }
}


void GStreamerVideo::draw()
{
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
        gst_element_call_async(playbin_, async_set_state_paused, this, nullptr);
    }
    else
    {
        gst_element_call_async(playbin_, async_set_state_playing, this, nullptr);
    }
}

void GStreamerVideo::restart()
{
    if (!isPlaying_)
        return;

    gst_element_call_async(playbin_, async_seek_to_start, nullptr, nullptr);
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
