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

void GlibLoop::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return; // already running
    }

    th_ = std::thread([this] {
        ctx_ = g_main_context_new();                 // dedicated context
        g_main_context_push_thread_default(ctx_);     // make it the thread-default
        loop_ = g_main_loop_new(ctx_, FALSE);

        g_main_loop_run(loop_);                       // blocks until quit

        g_main_loop_unref(loop_);
        loop_ = nullptr;
        g_main_context_pop_thread_default(ctx_);
        g_main_context_unref(ctx_);
        ctx_ = nullptr;

        running_.store(false, std::memory_order_release);
        });
}

void GlibLoop::stop() {
    if (!isRunning()) return;
    // schedule quit on the loop's thread
    g_main_context_invoke(context(),
        [](gpointer data) -> gboolean {
            auto* loop = static_cast<GMainLoop*>(data);
            g_main_loop_quit(loop);
            return G_SOURCE_REMOVE;
        },
        loop_);
    if (th_.joinable()) th_.join();
}

gboolean GlibLoop::invokeThunk(gpointer data) {
    // take ownership, run, then delete
    auto* fn = static_cast<std::function<void()>*>(data);
    (*fn)();
    delete fn;
    return G_SOURCE_REMOVE;
}

void GlibLoop::invoke(std::function<void()> fn, int priority) {
    if (!isRunning()) return;
    // allocate on heap; GLib will call invokeThunk on the loop thread
    auto* heapFn = new std::function<void()>(std::move(fn));
    g_main_context_invoke_full(context(), priority, &GlibLoop::invokeThunk,
        heapFn, /*notify*/ nullptr);
}

namespace {
    struct WaitCall {
        std::function<void()> fn;
        std::shared_ptr<std::promise<void>> pr;
    };

    static gboolean invokeWaitThunk(gpointer data) {
        auto* wc = static_cast<WaitCall*>(data);
        // run the user function on the GLib thread
        wc->fn();
        // signal completion
        wc->pr->set_value();
        delete wc;
        return G_SOURCE_REMOVE;
    }

    // Generic async call with result
    template<typename T>
    struct AsyncCall {
        std::function<T()> fn;
        std::shared_ptr<std::promise<T>> pr;
    };

    // Generic thunk for non-void types
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
        delete ac;
        return G_SOURCE_REMOVE;
    }

    // Specialized thunk for void type
    static gboolean invokeAsyncVoidThunk(gpointer data) {
        auto* ac = static_cast<AsyncCall<void>*>(data);
        try {
            ac->fn();
            ac->pr->set_value();  // No argument for void promise
        }
        catch (...) {
            ac->pr->set_exception(std::current_exception());
        }
        delete ac;
        return G_SOURCE_REMOVE;
    }
} // namespace

void GlibLoop::invokeAndWait(std::function<void()> fn, int priority) {
    if (!isRunning()) {
        // No loop? Just run inline.
        fn();
        return;
    }

    // If we're already on the GLib thread, run inline to avoid deadlock.
    if (th_.joinable() && std::this_thread::get_id() == th_.get_id()) {
        fn();
        return;
    }

    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();

    auto* wc = new WaitCall{ std::move(fn), pr };
    g_main_context_invoke_full(context(), priority, &invokeWaitThunk, wc, /*notify*/ nullptr);

    // block until the GLib thread has executed the task
    fut.get();
}

bool GlibLoop::invokeAndWaitFor(std::function<void()> fn,
    std::chrono::milliseconds timeout,
    int priority) {
    if (!isRunning()) {
        fn();
        return true;
    }
    if (th_.joinable() && std::this_thread::get_id() == th_.get_id()) {
        fn();
        return true;
    }

    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();

    auto* wc = new WaitCall{ std::move(fn), pr };
    g_main_context_invoke_full(context(), priority, &invokeWaitThunk, wc, /*notify*/ nullptr);

    return (fut.wait_for(timeout) == std::future_status::ready);
}

// Generic template method for async operations with return values
template<typename T>
std::future<T> GlibLoop::invokeAsync(std::function<T()> fn, int priority) {
    auto pr = std::make_shared<std::promise<T>>();
    auto fut = pr->get_future();

    if (!isRunning()) {
        try { pr->set_value(fn()); }
        catch (...) { pr->set_exception(std::current_exception()); }
        return fut;
    }

    // NEW: self-thread fast path
    if (th_.joinable() && std::this_thread::get_id() == th_.get_id()) {
        try { pr->set_value(fn()); }
        catch (...) { pr->set_exception(std::current_exception()); }
        return fut;
    }

    auto* ac = new AsyncCall<T>{ std::move(fn), pr };
    g_main_context_invoke_full(context(), priority, &invokeAsyncThunk<T>, ac, nullptr);
    return fut;
}

// Specialized template for void return type
template<>
std::future<void> GlibLoop::invokeAsync<void>(std::function<void()> fn, int priority) {
    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();

    if (!isRunning()) {
        // No loop? Run inline and return immediate future
        try {
            fn();
            pr->set_value();  // No argument for void promise
        }
        catch (...) {
            pr->set_exception(std::current_exception());
        }
        return fut;
    }

    auto* ac = new AsyncCall<void>{ std::move(fn), pr };
    g_main_context_invoke_full(context(), priority, &invokeAsyncVoidThunk, ac, /*notify*/ nullptr);

    return fut;
}

// Explicit template instantiations for common types
template std::future<bool> GlibLoop::invokeAsync<bool>(std::function<bool()>, int);
template std::future<int> GlibLoop::invokeAsync<int>(std::function<int()>, int);
// Note: void specialization is defined above, not instantiated

guint GlibLoop::addBusWatch(GstBus* bus, GstBusFunc func, gpointer user_data,
    GDestroyNotify notify, int priority) {
    if (!isRunning() || !bus || !func) return 0;
    std::promise<guint> pr;
    auto fut = pr.get_future();
    gst_object_ref(bus); // survive the hop
    invoke([bus, func, user_data, notify, &pr, priority]() mutable {
        // Attaches to THIS thread's thread-default context (set in start())
        guint id = gst_bus_add_watch_full(bus, priority, func, user_data, notify);
        pr.set_value(id);
        gst_object_unref(bus);
        }, priority);
    return fut.get();
}

void GlibLoop::removeSource(guint sourceId) {
    if (!isRunning() || sourceId == 0) return;
    invoke([sourceId]() { g_source_remove(sourceId); });
}