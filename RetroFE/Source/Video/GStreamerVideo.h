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
#include <mutex>
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

// Define TNQueue using a C-style array with padding
template<typename T, size_t N>
class TNQueue {
    // Ensure N is a power of two for efficient bitwise index wrapping
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

protected:
    alignas(CACHE_LINE_SIZE) T storage[N];  // C-style array for holding the buffers
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head;  // Atomic index for reading buffers
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail;  // Atomic index for writing new buffers
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> count; // Atomic count of items in the queue

private:
    std::mutex queueMutex_;

public:
    TNQueue() : head(0), tail(0), count(0) {}

    // Check if the queue is full
    bool isFull() const {
        return count.load(std::memory_order_acquire) == N;  // Full if count equals capacity
    }

    // Check if the queue is empty
    bool isEmpty() const {
        return count.load(std::memory_order_acquire) == 0;  // Empty if count is zero
    }

    // Push a new item into the queue
    void push(T item) {
        size_t currentTail = tail.load(std::memory_order_relaxed);

        if (isFull()) {
            LOG_DEBUG("TNQueue", "Queue is full. Dropping the oldest item.");
            auto droppedItemOpt = pop();  // Remove the oldest item
            if (droppedItemOpt.has_value()) {
                gst_clear_buffer(&(*droppedItemOpt));  // Use gst_clear_buffer to unref and clear the buffer
            }
        }

        storage[currentTail] = item;
        tail.store((currentTail + 1) & (N - 1), std::memory_order_release);  // Update tail with release semantics
        count.fetch_add(1, std::memory_order_release);  // Increment count
    }

    // Pop an item from the queue
    std::optional<T> pop() {
        size_t currentHead = head.load(std::memory_order_relaxed);

        if (isEmpty()) {
            return std::nullopt;  // Queue is empty
        }

        T item = storage[currentHead];
        head.store((currentHead + 1) & (N - 1), std::memory_order_release);  // Update head with release semantics
        count.fetch_sub(1, std::memory_order_release);  // Decrement count
        return item;
    }

    // Clear the queue
    void clear() {
        while (!isEmpty()) {
            auto itemOpt = pop();  // Use the pop() method to handle the queue logic

            if (itemOpt.has_value()) {
                gst_clear_buffer(&(*itemOpt));  // Safely clear and unref the buffer
            }
        }

        // Reset indices safely (not strictly necessary since pop() manages this, but good for consistency)
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        count.store(0, std::memory_order_relaxed);
    }

    // Get the current size of the queue
    size_t size() const {
        return count.load(std::memory_order_acquire);  // Directly return the count
    }
};

class GStreamerVideo final : public IVideo {
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
    GstClockTime getLastPTS() const override;
    GstClockTime getExpectedTime() const override;
    bool isNewFrameAvailable() const override;
    void resetNewFrameFlag() override;
    GstElement* getPipeline() const override;
    GstElement* getVideoSink() const override;
    static void enablePlugin(const std::string& pluginName);
    static void disablePlugin(const std::string& pluginName);

private:
    static void processNewBuffer(GstElement const* /* fakesink */, GstBuffer* buf, GstPad* new_pad,
        gpointer userdata);
    static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static void sourceSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
    static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static void initializePlugins();
    bool initializeGstElements(const std::string& file);
    void createSdlTexture();
    GstElement* playbin_{ nullptr };
    GstElement* videoSink_{ nullptr };
    GstElement* videoBin_{ nullptr };
    GstElement* capsFilter_{ nullptr };
    GstBus* videoBus_{ nullptr };
    GstClockTime baseTime_;
    GstVideoInfo* videoInfo_{ gst_video_info_new() };
    SDL_Texture* texture_{ nullptr };
    SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
    guint elementSetupHandlerId_{ 0 };
    guint sourceSetupHandlerId_{ 0 };
    guint handoffHandlerId_{ 0 };
    guint aboutToFinishHandlerId_{ 0 };
    guint padProbeId_{ 0 };
    guint prerollHandlerId_{ 0 };
    gint height_{ 0 };
    gint width_{ 0 };
    TNQueue<GstBuffer*, 8> bufferQueue_; // Using TNQueue to hold a maximum of 15 buffers
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
    std::atomic<bool> stopping_{ false };
    std::shared_mutex stopMutex_;
    static bool pluginsInitialized_;
    bool bufferDisconnected_{ true };
    GstClockTime lastPTS_;
    GstClockTime expectedTime_;
    bool newFrameAvailable_ = false;

    std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;
};

