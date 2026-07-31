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
#include "GlibLoop.h"
#include <SDL.h>
#include <stdexcept>

void GlibLoop::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return; // already running
    }

    // 1. FIX: Allocate BEFORE spawning the thread so they are instantly available
    ctx_ = g_main_context_new();
    loop_ = g_main_loop_new(ctx_, FALSE);

    th_ = std::thread([this] {
        if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW) != 0) {
            // Optionally log this once, though not critical
        }
        g_main_context_push_thread_default(ctx_);

        g_main_loop_run(loop_); // blocks until quit

        g_main_context_pop_thread_default(ctx_);

        // Teardown
        running_.store(false, std::memory_order_release);

        g_main_loop_unref(loop_);
        loop_ = nullptr;
        g_main_context_unref(ctx_);
        ctx_ = nullptr;
        });
}

void GlibLoop::stop() {
    if (!isRunning()) return;

    // 3. FIX: g_main_loop_quit is thread-safe. Call it directly to instantly unblock.
    if (loop_) {
        g_main_loop_quit(loop_);
    }

    if (th_.joinable()) {
        th_.join();
    }
}

// --- BASIC INVOKE ---
namespace {
    struct InvokeCall {
        std::function<void()> fn;
    };

    static gboolean invokeThunk(gpointer data) {
        auto* ic = static_cast<InvokeCall*>(data);
        ic->fn();
        return G_SOURCE_REMOVE;
    }

    static void invokeDestroy(gpointer data) {
        delete static_cast<InvokeCall*>(data);
    }
}

void GlibLoop::invoke(std::function<void()> fn, int priority) {
    if (!isRunning()) return;
    auto* ic = new InvokeCall{ std::move(fn) };

    // 2. FIX: Always pass the destroy_notify to prevent leaks
    g_main_context_invoke_full(context(), priority, &invokeThunk, ic, &invokeDestroy);
}

// --- WAIT CALL ---
namespace {
    struct WaitCall {
        std::function<void()> fn;
        std::shared_ptr<std::promise<void>> pr;
        bool executed = false; // Track if GLib aborted us
    };

    static gboolean invokeWaitThunk(gpointer data) {
        auto* wc = static_cast<WaitCall*>(data);
        try {
            wc->fn();
            wc->pr->set_value();
        }
        catch (...) {
            wc->pr->set_exception(std::current_exception());
        }
        wc->executed = true;
        return G_SOURCE_REMOVE;
    }

    static void invokeWaitDestroy(gpointer data) {
        auto* wc = static_cast<WaitCall*>(data);
        if (!wc->executed) {
            // FIX: Prevent deadlocks if the loop shuts down before executing
            wc->pr->set_exception(std::make_exception_ptr(std::runtime_error("GlibLoop aborted task")));
        }
        delete wc;
    }
}

void GlibLoop::invokeAndWait(std::function<void()> fn, int priority) {
    if (!isRunning()) { fn(); return; }
    if (th_.joinable() && std::this_thread::get_id() == th_.get_id()) { fn(); return; }

    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();
    auto* wc = new WaitCall{ std::move(fn), pr, false };

    g_main_context_invoke_full(context(), priority, &invokeWaitThunk, wc, &invokeWaitDestroy);
    fut.get(); // Now 100% guaranteed to return or throw, never hang
}

bool GlibLoop::invokeAndWaitFor(std::function<void()> fn, std::chrono::milliseconds timeout, int priority) {
    if (!isRunning()) { fn(); return true; }
    if (th_.joinable() && std::this_thread::get_id() == th_.get_id()) { fn(); return true; }

    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();
    auto* wc = new WaitCall{ std::move(fn), pr, false };

    g_main_context_invoke_full(context(), priority, &invokeWaitThunk, wc, &invokeWaitDestroy);
    return (fut.wait_for(timeout) == std::future_status::ready);
}

// --- ASYNC CALLS ---
namespace {
    template<typename T>
    struct AsyncCall {
        std::function<T()> fn;
        std::shared_ptr<std::promise<T>> pr;
        bool executed = false;
    };

    template<typename T>
    static gboolean invokeAsyncThunk(gpointer data) {
        auto* ac = static_cast<AsyncCall<T>*>(data);
        try {
            T result = ac->fn();
            ac->pr->set_value(result);
        }
        catch (...) {
            ac->pr->set_exception(std::current_exception());
        }
        ac->executed = true;
        return G_SOURCE_REMOVE;
    }

    static gboolean invokeAsyncVoidThunk(gpointer data) {
        auto* ac = static_cast<AsyncCall<void>*>(data);
        try {
            ac->fn();
            ac->pr->set_value();
        }
        catch (...) {
            ac->pr->set_exception(std::current_exception());
        }
        ac->executed = true;
        return G_SOURCE_REMOVE;
    }

    template<typename T>
    static void invokeAsyncDestroy(gpointer data) {
        auto* ac = static_cast<AsyncCall<T>*>(data);
        if (!ac->executed) {
            ac->pr->set_exception(std::make_exception_ptr(std::runtime_error("GlibLoop aborted async task")));
        }
        delete ac;
    }
}

template<typename T>
std::future<T> GlibLoop::invokeAsync(std::function<T()> fn, int priority) {
    auto pr = std::make_shared<std::promise<T>>();
    auto fut = pr->get_future();

    if (!isRunning() || (th_.joinable() && std::this_thread::get_id() == th_.get_id())) {
        try { pr->set_value(fn()); }
        catch (...) { pr->set_exception(std::current_exception()); }
        return fut;
    }

    auto* ac = new AsyncCall<T>{ std::move(fn), pr, false };
    g_main_context_invoke_full(context(), priority, &invokeAsyncThunk<T>, ac, &invokeAsyncDestroy<T>);
    return fut;
}

template<>
std::future<void> GlibLoop::invokeAsync<void>(std::function<void()> fn, int priority) {
    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();

    if (!isRunning() || (th_.joinable() && std::this_thread::get_id() == th_.get_id())) {
        try { fn(); pr->set_value(); }
        catch (...) { pr->set_exception(std::current_exception()); }
        return fut;
    }

    auto* ac = new AsyncCall<void>{ std::move(fn), pr, false };
    g_main_context_invoke_full(context(), priority, &invokeAsyncVoidThunk, ac, &invokeAsyncDestroy<void>);
    return fut;
}

template std::future<bool> GlibLoop::invokeAsync<bool>(std::function<bool()>, int);
template std::future<int> GlibLoop::invokeAsync<int>(std::function<int()>, int);

guint GlibLoop::addBusWatch(GstBus* bus, GstBusFunc func, gpointer user_data, GDestroyNotify notify, int priority) {
    if (!isRunning() || !bus || !func) {
        // Match the ownership transfer of gst_bus_add_watch_full even when
        // the watch cannot be installed.
        if (notify && user_data) {
            notify(user_data);
        }
        return 0;
    }

    auto pr = std::make_shared<std::promise<guint>>();
    auto fut = pr->get_future();

    gst_object_ref(bus); // <--- Keep it alive for the jump

    invoke([bus, func, user_data, notify, pr, priority]() mutable {
        guint id = gst_bus_add_watch_full(bus, priority, func, user_data, notify);
        pr->set_value(id);
        gst_object_unref(bus); // <--- Done with it
        }, priority);

    return fut.get();
}

void GlibLoop::removeSource(guint sourceId) {
    if (!isRunning() || sourceId == 0) return;
    invoke([sourceId]() {
        GMainContext* ctx = GlibLoop::instance().context();
        GSource* source = g_main_context_find_source_by_id(ctx, sourceId);
        if (source) {
            g_source_destroy(source);
        }
        });
}
