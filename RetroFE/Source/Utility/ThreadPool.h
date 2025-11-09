#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>      // for std::future, std::packaged_task
#include <functional>  // for std::invoke
#include <stdexcept>
#include <type_traits> // for std::invoke_result_t, std::is_void_v
#include <utility>     // for std::move, std::forward
#include <memory>      // for std::shared_ptr
#include <tuple>       // for std::tuple, std::make_tuple, std::apply

#include "Log.h"

class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    void shutdown();
    // A thread pool is a unique resource. It should not be copied or moved.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Singleton accessor
    static ThreadPool& getInstance();

    void wait();

    // Enqueue tasks to the thread pool
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>;

private:
    // Worker threads
    std::vector<std::thread> workers;
    // Task queue
    std::queue<std::function<void()>> tasks;

    // Synchronization
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
    std::condition_variable waitCondition;
    size_t activeWorkers = 0;
};

// Implementation of the enqueue method needs to be visible to all translation units that use it, hence defined in the header
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    // Store callable and args by value (moved), invoke via std::apply
    auto pkg = std::make_shared<std::packaged_task<return_type()>>(
        [func = std::forward<F>(f),
        tup = std::make_tuple(std::forward<Args>(args)...)
        ]() mutable -> return_type {
            if constexpr (std::is_void_v<return_type>) {
                std::apply(std::move(func), std::move(tup));
                return;
            }
            else {
                return std::apply(std::move(func), std::move(tup));
            }
        }
    );

    std::future<return_type> res = pkg->get_future();
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
        tasks.emplace([pkg] { (*pkg)(); });
    }
    condition.notify_one();
    return res;
}


#endif // THREADPOOL_H
