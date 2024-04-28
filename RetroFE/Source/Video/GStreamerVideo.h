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
#include "../Database/Configuration.h"
#include "../Utility/Utils.h"
extern "C"
{
#if (__APPLE__)
    #include <GStreamer/gst/gst.h>
    #include <GStreamer/gst/video/gstvideometa.h>
#else
    #include <gst/gst.h>
    #include <gst/video/gstvideometa.h>



#endif

}


class GStreamerVideo final : public IVideo
{
public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo&) = delete;
    GStreamerVideo& operator=(const GStreamerVideo&) = delete;
    ~GStreamerVideo() override;
    bool initialize() override;
    bool play(const std::string& file) override;
    bool stop() override;
    bool deInitialize() override;
    SDL_Texture* getTexture() const override;
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
    bool getFrameReady();
    // Helper functions...
    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);

private:
    enum BufferLayout {
        UNKNOWN,        // Initial state
        CONTIGUOUS,     // Contiguous buffer layout
        NON_CONTIGUOUS,  // Non-contiguous buffer layout
    };

    static void processNewBuffer(GstElement const*/* fakesink */, GstBuffer* buf, GstPad* new_pad, gpointer userdata);
    static void elementSetupCallback([[maybe_unused]] GstElement const* playbin, GstElement* element, [[maybe_unused]] GStreamerVideo const* video);
    bool initializeGstElements(const std::string& file);
    GstElement* playbin_{ nullptr };
    GstElement* videoBin_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* capsFilter_{ nullptr };
    GstBus* videoBus_{ nullptr };
    SDL_Texture* texture_{ nullptr };
    gulong elementSetupHandlerId_{ 0 };
    gulong handoffHandlerId_{ 0 };
    gint height_{ 0 };
    gint width_{ 0 };
    GstBuffer* videoBuffer_{ nullptr };
    const GstVideoMeta* videoMeta_{ nullptr };
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
    unsigned int expectedBufSize_{ 0 };

    bool useD3dHardware_{ Configuration::HardwareVideoAccel
        && SDL::getRendererBackend(0) == "direct3d11" };
    bool useVaHardware_{ Configuration::HardwareVideoAccel
        && SDL::getRendererBackend(0) == "opengl"
        && Utils::getOSType() == "linux" };

    BufferLayout bufferLayout_{ UNKNOWN };
    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath);

};