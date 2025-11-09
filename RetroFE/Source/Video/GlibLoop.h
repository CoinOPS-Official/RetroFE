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
#include <glib.h>
#include <gst/gst.h>
#include <thread>
#include <atomic>
#include <functional>
#include <future>

class GlibLoop {
public:
    static GlibLoop& instance() {
        static GlibLoop s;
        return s;
    }

    template<typename T>
    std::future<T> invokeAsync(std::function<T()> fn, int priority = G_PRIORITY_DEFAULT);

    // Start a dedicated context + loop thread (idempotent).
    void start();

    // Gracefully stop the loop and join the thread (idempotent).
    void stop();

    // Is the loop running?
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Access the loop's context (nullptr if not started).
    GMainContext* context() const { return ctx_; }

    // Run a function on the loop thread ASAP.
    // Non-blocking: returns immediately.
    void invoke(std::function<void()> fn, int priority = G_PRIORITY_DEFAULT);

    void invokeAndWait(std::function<void()> fn, int priority = G_PRIORITY_DEFAULT);

    bool invokeAndWaitFor(std::function<void()> fn,
        std::chrono::milliseconds timeout,
        int priority = G_PRIORITY_DEFAULT);

    // Convenience: attach a GstBus watch to THIS loop, from any thread.
    // Returns the source ID (0 on failure). This call blocks until attached.
    guint addBusWatch(GstBus* bus,
        GstBusFunc func,
        gpointer user_data,
        GDestroyNotify notify = nullptr,
        int priority = G_PRIORITY_DEFAULT);

    // Convenience remove (safe to call from any thread).
    void removeSource(guint sourceId);

private:
    GlibLoop() = default;
    ~GlibLoop() = default;
    GlibLoop(const GlibLoop&) = delete;
    GlibLoop& operator=(const GlibLoop&) = delete;

    static gboolean invokeThunk(gpointer data); // internal

    GMainContext* ctx_ = nullptr;
    GMainLoop* loop_ = nullptr;
    std::thread   th_;
    std::atomic<bool> running_{ false };
};