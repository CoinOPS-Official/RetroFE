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

extern "C"
{
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
}


class GStreamerVideo : public IVideo
{
public:
    explicit GStreamerVideo( int monitor );
    ~GStreamerVideo();
    bool initialize();
    bool play(const std::string& file);
    bool stop();
    bool deInitialize();
    SDL_Texture *getTexture() const;
    void update(float dt);
    void volumeUpdate();
    void draw();
    void setNumLoops(int n);
    void freeElements();
    int getHeight();
    int getWidth();
    bool isPlaying();
    void setVolume(float volume);
    void skipForward( );
    void skipBackward( );
    void skipForwardp( );
    void skipBackwardp( );
    void pause( );
    void restart( );
    unsigned long long getCurrent( );
    unsigned long long getDuration( );
    bool isPaused( );

private:
    enum BufferLayout {
        UNKNOWN,        // Initial state
        CONTIGUOUS,     // Contiguous buffer layout
        NON_CONTIGUOUS  // Non-contiguous buffer layout
    };
    
    static void processNewBuffer (GstElement const *fakesink, GstBuffer *buf, GstPad *pad, gpointer data);
    static void elementSetupCallback([[maybe_unused]] GstElement const* playbin, GstElement* element, [[maybe_unused]] GStreamerVideo const* video);
    bool initializeGstElements(const std::string& file);
    bool createAndLinkGstElements();
    void loopHandler();
    GstElement* playbin_{ nullptr };
    GstElement* videoBin_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* videoConvert_{ nullptr };
    GstElement* capsFilter_{ nullptr };
    GstCaps* videoConvertCaps_{ nullptr };
    GstBus* videoBus_{ nullptr };
    SDL_Texture* texture_{ nullptr };
    gulong elementSetupHandlerId_{ 0 };
    gulong handoffHandlerId_{ 0 };
    gint height_{ 0 };
    gint width_{ 0 };
    GstBuffer* videoBuffer_{ nullptr };
    bool frameReady_{ false };
    bool isPlaying_{ false };
    static bool initialized_;
    int playCount_{ 0 };
    std::string currentFile_{};
    int numLoops_{ 0 };
    float volume_{ 0.0f };
    double currentVolume_{ 0.0 };
    int monitor_;
    bool paused_{ false };
    double lastSetVolume_{ 0.0 };
    bool lastSetMuteState_{ false };
    BufferLayout bufferLayout_{ UNKNOWN };
};