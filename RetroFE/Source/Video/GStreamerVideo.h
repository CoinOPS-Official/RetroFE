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


// Define cache line size (common value for x86/x64 architectures)
constexpr size_t CACHE_LINE_SIZE = 64;

template<typename T, size_t N>
class TNQueue {
    // Ensure N is a power of two for efficient bitwise index wrapping
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

protected:
    alignas(CACHE_LINE_SIZE) T storage[N];  // C-style array for holding the buffers
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head;  // Atomic index for reading buffers
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail;  // Atomic index for writing new buffers

public:
    TNQueue() : head(0), tail(0) {
        for (size_t i = 0; i < N; ++i) {
            storage[i] = T{};
        }
    }

    bool isFull() const {
        size_t currentTail = tail.load(std::memory_order_acquire);
        size_t currentHead = head.load(std::memory_order_acquire);
        return ((currentTail + 1) & (N - 1)) == currentHead;
    }

    bool isEmpty() const {
        size_t currentTail = tail.load(std::memory_order_acquire);
        size_t currentHead = head.load(std::memory_order_acquire);
        return currentHead == currentTail;
    }

    bool push(T&& item) {
        size_t currentTail = tail.load(std::memory_order_relaxed);
        size_t nextTail = (currentTail + 1) & (N - 1);

        if (nextTail == head.load(std::memory_order_acquire)) {
            // Queue is full, drop oldest item
            size_t currentHead = head.load(std::memory_order_relaxed);
            storage[currentHead] = T{};  // Replace with empty item, triggering cleanup
            head.store((currentHead + 1) & (N - 1), std::memory_order_release);
        }

        storage[currentTail] = std::move(item);
        tail.store(nextTail, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        size_t currentHead = head.load(std::memory_order_relaxed);

        if (currentHead == tail.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        T item = std::move(storage[currentHead]);
        storage[currentHead] = T{};  // Replace with empty item
        head.store((currentHead + 1) & (N - 1), std::memory_order_release);
        return std::move(item);
    }

    size_t size() const {
        size_t currentHead = head.load(std::memory_order_acquire);
        size_t currentTail = tail.load(std::memory_order_acquire);
        return (currentTail + N - currentHead) & (N - 1);
    }
};


class GStreamerVideo final : public IVideo {
public:
    explicit GStreamerVideo(int monitor);
    GStreamerVideo(const GStreamerVideo&) = delete;
    GStreamerVideo& operator=(const GStreamerVideo&) = delete;
    ~GStreamerVideo() override;
    bool initialize() override;
    bool unload();
    bool createPipelineIfNeeded();
    bool play(const std::string& file) override;
    bool stop() override;
    bool deInitialize() override;
    SDL_Texture* getTexture() const override;
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
    void bufferDisconnect(bool disconnect) override;
    bool isBufferDisconnected() override;
    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);

    void setSoftOverlay(bool value);

private:


    // MappedFrameData implementation (place in GStreamerVideo class private section)
    struct MappedFrameData {
        GstVideoFrame frame;  // The actual mapped frame
        bool is_mapped;       // Track mapping state

        MappedFrameData() : is_mapped(false) {}

        // Move constructor
        MappedFrameData(MappedFrameData&& other) noexcept 
            : frame(other.frame), is_mapped(other.is_mapped) {
            other.is_mapped = false;
        }

        // Move assignment
        MappedFrameData& operator=(MappedFrameData&& other) noexcept {
            if (this != &other) {
                if (is_mapped) {
                    gst_video_frame_unmap(&frame);
                }
                frame = other.frame;
                is_mapped = other.is_mapped;
                other.is_mapped = false;
            }
            return *this;
        }

        ~MappedFrameData() {
            if (is_mapped) {
                gst_video_frame_unmap(&frame);
            }
        }

        MappedFrameData(const MappedFrameData&) = delete;
        MappedFrameData& operator=(const MappedFrameData&) = delete;
    };

    static void processNewBuffer(GstElement const* /* fakesink */, GstBuffer* buf, GstPad* new_pad,
        gpointer userdata);
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static void initializePlugins();
    void createSdlTexture();
    GstElement* playbin_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* videoBin_{ nullptr };
    GstElement* capsFilter_{ nullptr };
    GstBus* videoBus_{ nullptr };
    GstVideoInfo videoInfo_;
    SDL_Texture* texture_{ nullptr };
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    guint elementSetupHandlerId_{ 0 };
    guint handoffHandlerId_{ 0 };
    guint padProbeId_{ 0 };
    std::atomic<int> width_{ 0 };
    std::atomic<int> height_{ 0 };
	int textureWidth_{ -1 };
	int textureHeight_{ -1 };
	bool textureValid_{ false };
    TNQueue<MappedFrameData, 8> frameQueue_; // Using TNQueue to hold a maximum of 8 buffers
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
    bool bufferDisconnected_{ true };
    bool softOverlay_;

    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;
};