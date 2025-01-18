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
    gst_video_info_init(&videoInfo_);
    initializePlugins();
}


    GStreamerVideo::~GStreamerVideo() = default;


void GStreamerVideo::initializePlugins()
{
    if (!pluginsInitialized_)
    {
        pluginsInitialized_ = true;

#if defined(WIN32)
        enablePlugin("directsoundsink");
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
		//enablePlugin("pipewiresink");
        //disablePlugin("alsasink");
        //disablePlugin("pulsesink");
        if (Configuration::HardwareVideoAccel)
        {
            enablePlugin("vah264dec");
            enablePlugin("vah265dec");
        }
        if (!Configuration::HardwareVideoAccel)
        {
            disablePlugin("vah264dec");
            disablePlugin("vah265dec");
            //enablePlugin("openh264dec");
            enablePlugin("avdec_h264");
            enablePlugin("avdec_h265");
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

        // Clear the buffer queue
        bufferQueue_.clear();

        // Disconnect signal handlers
        if (elementSetupHandlerId_)
        {
            g_signal_handler_disconnect(playbin_, elementSetupHandlerId_);
            elementSetupHandlerId_ = 0;
        }

        if (handoffHandlerId_)
        {
            g_signal_handler_disconnect(videoSink_, handoffHandlerId_);
            handoffHandlerId_ = 0;
        }


        gst_object_unref(playbin_);

        playbin_ = nullptr;
        videoSink_ = nullptr;
        capsFilter_ = nullptr;
        videoBin_ = nullptr;
        videoBus_ = nullptr;



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


bool GStreamerVideo::unload()
{
    // If we never created playbin_, nothing to unload
    if (!playbin_) {
        return false;
    }

    // Optionally mark it as “stopping” so other threads know not to process more buffers
    stopping_.store(true, std::memory_order_release);

    bufferDisconnect(true);

    // Set pipeline to GST_STATE_READY (instead of GST_STATE_NULL) so we can reuse it later
    GstStateChangeReturn ret = gst_element_set_state(playbin_, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("GStreamerVideo", "Failed to set pipeline to READY during unload.");
        return false;
    }

    // Optionally wait for the state change to complete 
    GstState newState;
    ret = gst_element_get_state(playbin_, &newState, nullptr, GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE || newState != GST_STATE_READY) {
        LOG_ERROR("GStreamerVideo", "Pipeline did not reach READY state during unload.");
    }

    // We’re no longer “playing”
    isPlaying_ = false;

    // Clear any queued buffers
    bufferQueue_.clear();

    // Reset flags used for timing, volume, etc.
    paused_ = false;
    currentVolume_ = 0.0f;
    lastSetVolume_ = -1.0f;
    lastSetMuteState_ = false;
    volume_ = 0.0f;            // reset to default
    
    gst_video_info_init(&videoInfo_);
    textureWidth_ = width_;
    textureHeight_ = height_;
    width_ = 0;
    height_ = 0;
    playCount_ = 0;
    numLoops_ = 0;

    // Let future calls proceed
    stopping_.store(false, std::memory_order_release);

    LOG_DEBUG("GStreamerVideo", "Pipeline unloaded, now in READY state. Texture cleared.");

    return true;
}

bool GStreamerVideo::createPipelineIfNeeded()
{
    // If we already have a playbin_, just return true
    if (playbin_) {
        return true;
    }

    // Otherwise, create playbin, set up caps, videoSink, etc.
    playbin_   = gst_element_factory_make("playbin3", "player");
    capsFilter_ = gst_element_factory_make("capsfilter", "caps_filter");
    videoBin_  = gst_bin_new("SinkBin");
    videoSink_ = gst_element_factory_make("fakesink", "video_sink");

    if (!playbin_ || !videoBin_ || !videoSink_ || !capsFilter_) {
        LOG_DEBUG("Video", "Could not create GStreamer elements");
        return false;
    }

    gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
    g_object_set(playbin_, "flags", flags, nullptr);


    g_object_set(playbin_, "instant-uri", TRUE, nullptr);


    // Configure caps (hardware accel or software)
    GstCaps *videoConvertCaps;
    if (Configuration::HardwareVideoAccel) {
        videoConvertCaps = gst_caps_from_string("video/x-raw,format=(string)NV12,pixel-aspect-ratio=(fraction)1/1");
        sdlFormat_ = SDL_PIXELFORMAT_NV12;
        LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_NV12 (HW accel: true)");
    } else {
        videoConvertCaps = gst_caps_from_string("video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1");
        elementSetupHandlerId_ = g_signal_connect(playbin_, "element-setup", G_CALLBACK(elementSetupCallback), this);
        sdlFormat_ = SDL_PIXELFORMAT_IYUV;
        LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_IYUV (HW accel: false)");
    }

    g_object_set(capsFilter_, "caps", videoConvertCaps, nullptr);
    gst_caps_unref(videoConvertCaps);

    gst_bin_add_many(GST_BIN(videoBin_), capsFilter_, videoSink_, nullptr);
    if (!gst_element_link_many(capsFilter_, videoSink_, nullptr)) {
        LOG_DEBUG("Video", "Could not link video processing elements");
        return false;
    }

    GstPad *sinkPad   = gst_element_get_static_pad(capsFilter_, "sink");
    GstPad *ghostPad  = gst_ghost_pad_new("sink", sinkPad);
    gst_element_add_pad(videoBin_, ghostPad);
    gst_object_unref(sinkPad);

    // We haven't set URI yet; just attach the videoBin as playbin's video-sink
    g_object_set(playbin_, "video-sink", videoBin_, nullptr);

    videoBus_ = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
    gst_object_unref(videoBus_);

    g_object_set(videoSink_, "sync", TRUE, "enable-last-sample", FALSE, nullptr);
    bufferDisconnected_ = true;

    handoffHandlerId_ = g_signal_connect(videoSink_, "handoff", G_CALLBACK(processNewBuffer), this);


    return true;
}


bool GStreamerVideo::play(const std::string &file)
{
    playCount_ = 0;
    if (!initialized_) {
        return false;
    }

    // 1. Create the pipeline if we haven’t already
    if (!createPipelineIfNeeded()) {
        LOG_ERROR("Video", "Failed to create GStreamer pipeline");
        return false;
    }

    // reconnect the pad probe if it's not connected.
    if (GstPad* pad = gst_element_get_static_pad(videoSink_, "sink")) {
        // If padProbeId_ is non-zero, you may want to remove the old probe first.
        if (padProbeId_ != 0) {
            gst_pad_remove_probe(pad, padProbeId_);
            padProbeId_ = 0;
        }
        padProbeId_ = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
            padProbeCallback, this, nullptr);
        gst_object_unref(pad);
    }

    // 2. Move pipeline to READY (so we can safely change URI)
    //gst_element_set_state(playbin_, GST_STATE_READY);

    // 3. Convert file path to URI
    gchar* uriFile = gst_filename_to_uri(file.c_str(), nullptr);
    if (!uriFile) {
        LOG_DEBUG("Video", "Failed to convert filename to URI");
        return false;
    }
    GstState current, pending;
    gst_element_get_state(playbin_, &current, &pending, 0);

    // Update URI - no need to set to READY first
    g_object_set(playbin_, "uri", uriFile, nullptr);
    g_free(uriFile);

    // If we're not already playing, start playback
    if (current != GST_STATE_PLAYING) {
        GstStateChangeReturn playState = gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING);
        if (playState != GST_STATE_CHANGE_ASYNC && playState != GST_STATE_CHANGE_SUCCESS) {
            isPlaying_ = false;
            LOG_ERROR("Video", "Unable to set pipeline to the playing state.");
            stop();
            return false;
        }
    }

    paused_   = false;
    isPlaying_= true;
    currentFile_ = file;

    // Mute and volume to 0 by default
    gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_), GST_STREAM_VOLUME_FORMAT_LINEAR, 0.0);
    gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), true);
    lastSetMuteState_ = true;

    // Optionally wait for PLAYING state if you want to confirm it's active
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

    LOG_DEBUG("GStreamerVideo", "Playing file: " + file);
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
    capsFilter_ = gst_element_factory_make("capsfilter", "caps_filter");
    videoBin_ = gst_bin_new("SinkBin");
    videoSink_ = gst_element_factory_make("fakesink", "video_sink");

    if (!playbin_ || !videoBin_ || !videoSink_ || !capsFilter_)
    {
        LOG_DEBUG("Video", "Could not create GStreamer elements");
        g_free(uriFile);
        return false;
    }

    gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
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

void GStreamerVideo::elementSetupCallback([[maybe_unused]] GstElement* playbin, GstElement* element, [[maybe_unused]] gpointer data)
{
    // Check if the element is a video decoder
    if (!Configuration::HardwareVideoAccel && GST_IS_VIDEO_DECODER(element))
    {
            // Configure the video decoder
            g_object_set(element, "thread-type", Configuration::AvdecThreadType,
                "max-threads", Configuration::AvdecMaxThreads,
                "direct-rendering", FALSE, nullptr);
    }
}

GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    auto* video = static_cast<GStreamerVideo*>(user_data);

    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS)
    {
        GstCaps* caps = nullptr;
        gst_event_parse_caps(event, &caps);
        if (caps)
        {
            if (gst_video_info_from_caps(&video->videoInfo_, caps))
            {
                int newWidth = video->videoInfo_.width;
                int newHeight = video->videoInfo_.height;
                // Optionally, you can check if these are nonzero before proceeding.
                if (newWidth > 0 && newHeight > 0) {
                    video->width_.store(newWidth, std::memory_order_release);
                    video->height_.store(newHeight, std::memory_order_release);
                    LOG_DEBUG("GStreamerVideo", "Caps received, video dimensions: "
                        + std::to_string(newWidth) + "x" + std::to_string(newHeight));
                }
                else {
                    LOG_DEBUG("GStreamerVideo", "Received caps with invalid dimensions.");
                }
            }
        }
        // Now remove the probe so that this callback is not triggered again for this video.
        gst_pad_remove_probe(pad, video->padProbeId_);
        video->padProbeId_ = 0;  // reset the probe id for future use
    }
    return GST_PAD_PROBE_OK;
}


void GStreamerVideo::createSdlTexture() {
    int newWidth = width_.load(std::memory_order_acquire);
    int newHeight = height_.load(std::memory_order_acquire);

    // Validate the new dimensions.
    if (newWidth <= 0 || newHeight <= 0) {
        LOG_ERROR("GStreamerVideo", "Invalid dimensions (" +
            std::to_string(newWidth) + "x" + std::to_string(newHeight) + ").");
        textureValid_ = false;
        return;
    }

    // If we have an existing texture but its dimensions don't match, destroy it.
    if (texture_ && (textureWidth_ != newWidth || textureHeight_ != newHeight)) {
        SDL_LockMutex(SDL::getMutex());
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
        SDL_UnlockMutex(SDL::getMutex());
        textureValid_ = false;
    }

    // Only create a new texture if we don't have one.
    if (!texture_) {
        SDL_Texture* newTexture = SDL_CreateTexture(
            SDL::getRenderer(monitor_), sdlFormat_,
            SDL_TEXTUREACCESS_STREAMING, newWidth, newHeight);

        if (!newTexture) {
            LOG_ERROR("GStreamerVideo", "SDL_CreateTexture failed: " + std::string(SDL_GetError()));
            textureValid_ = false;
            return;
        }

        SDL_BlendMode blendMode = softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND;
        if (SDL_SetTextureBlendMode(newTexture, blendMode) != 0) {
            LOG_ERROR("GStreamerVideo", "SDL_SetTextureBlendMode failed: " + std::string(SDL_GetError()));
            SDL_DestroyTexture(newTexture);
            textureValid_ = false;
            return;
        }

        SDL_LockMutex(SDL::getMutex());
        texture_ = newTexture;
        SDL_UnlockMutex(SDL::getMutex());
    }

    // Update the cached dimensions and mark the texture as valid.
    textureWidth_ = newWidth;
    textureHeight_ = newHeight;
    textureValid_ = true;

    LOG_DEBUG("GStreamerVideo", "Texture created or updated with dimensions (" +
        std::to_string(newWidth) + "x" + std::to_string(newHeight) + ").");
}

void GStreamerVideo::loopHandler() {
    if (!videoBus_ || !isPlaying_)
        return;

    // Process all pending messages
    GstMessage* msg;
    while ((msg = gst_bus_pop_filtered(videoBus_,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)))) {

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            playCount_++;
            if (!numLoops_ || numLoops_ > playCount_) {
                restart();
            }
            else {
                stop();
            }
            break;

        case GST_MESSAGE_ERROR:
            GError* err;
            gchar* debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            LOG_ERROR("GStreamerVideo", "Error received from element " +
                std::string(GST_OBJECT_NAME(msg->src)) + ": " +
                std::string(err->message));
            g_clear_error(&err);
            g_free(debug_info);
            break;
        

        default:
            // Ignore other message types
            break;
        }

        gst_message_unref(msg);
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
    if (!isPlaying_ || !playbin_)
        return;

    // Clamp volume_ to valid range
    volume_ = std::min(volume_, 1.0f);

    // Gradually adjust currentVolume_ towards volume_
    if (currentVolume_ > volume_ || currentVolume_ + 0.005 >= volume_)
        currentVolume_ = volume_;
    else
        currentVolume_ += 0.005;

    // Determine mute state
    bool shouldMute = (currentVolume_ < 0.1);

    // Update volume only if it has changed and is not muted
    if (!shouldMute && currentVolume_ != lastSetVolume_)
    {
        gst_stream_volume_set_volume(
            GST_STREAM_VOLUME(playbin_), 
            GST_STREAM_VOLUME_FORMAT_LINEAR, 
            currentVolume_);
        lastSetVolume_ = currentVolume_;
    }

    // Update mute state only if it has changed
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

void GStreamerVideo::processNewBuffer(GstElement const* /* fakesink */, GstBuffer* buf, [[maybe_unused]] GstPad* new_pad, gpointer userdata) {
    auto* video = static_cast<GStreamerVideo*>(userdata);

    if (video->stopping_.load(std::memory_order_acquire)) {
        return;
    }

    // No need for separate ref/unref operations
    if (!video->bufferQueue_.pushWithRef(buf)) {
        // If push fails (shouldn't happen with current implementation)
        // No need to unref as we never incremented the reference
        return;
    }
}

void GStreamerVideo::draw() {
    // Only draw if isPlaying_ is true. (Assuming isPlaying_ is now atomic.)
    if (!isPlaying_)
        return;

    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }

    auto bufferOpt = bufferQueue_.pop();
    if (!bufferOpt.has_value()) {
        return;
    }

    // Scope the buffer using RAII
    ScopedBuffer scopedBuffer(*bufferOpt);

    if (!textureValid_ ||
        width_.load(std::memory_order_acquire) != textureWidth_ ||
        height_.load(std::memory_order_acquire) != textureHeight_)
    {
        createSdlTexture();
    }

    if (texture_) {
        ScopedVideoFrame videoFrame(&videoInfo_, scopedBuffer.get());

        if (videoFrame.isMapped()) {
            SDL_LockMutex(SDL::getMutex());

            if (sdlFormat_ == SDL_PIXELFORMAT_NV12) {
                if (SDL_UpdateNVTexture(texture_, nullptr,
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(videoFrame.get(), 0)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(videoFrame.get(), 0),
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(videoFrame.get(), 1)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(videoFrame.get(), 1)) != 0) {
                    LOG_ERROR("GStreamerVideo", "Unable to update NV texture: " + std::string(SDL_GetError()));
                }
            }
            else if (sdlFormat_ == SDL_PIXELFORMAT_IYUV) {
                if (SDL_UpdateYUVTexture(texture_, nullptr,
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(videoFrame.get(), 0)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(videoFrame.get(), 0),
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(videoFrame.get(), 1)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(videoFrame.get(), 1),
                    static_cast<const Uint8*>(GST_VIDEO_FRAME_PLANE_DATA(videoFrame.get(), 2)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(videoFrame.get(), 2)) != 0) {
                    LOG_ERROR("GStreamerVideo", "Unable to update YUV texture: " + std::string(SDL_GetError()));
                }
            }

            SDL_UnlockMutex(SDL::getMutex());
        }
    }
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

    // Clear buffered frames
    bufferQueue_.clear();


    // Use same seeking method consistently
    if (!gst_element_seek(playbin_, 1.0, GST_FORMAT_TIME, 
        GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET, 0,
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
        LOG_ERROR("GStreamerVideo", "Failed to seek to start");
    }
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