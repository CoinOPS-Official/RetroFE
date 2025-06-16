#include "ThreadPool.h"
#include "Log.h"
#include <cstdlib>   // for std::getenv
#include <algorithm> // for std::clamp

// Constructor
ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for (size_t i = 0; i < threads; ++i)
        workers.emplace_back(
            [this] {
                for (;;) {
                    std::function<void()> task; {
                        std::unique_lock<std::mutex> lock(this->queueMutex);
                        this->condition.wait(lock,
                            [this] { return this->stop || !this->tasks.empty(); });

                        if (this->stop && this->tasks.empty())
                            return;

                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }

                    task();
                }
            }
    );
}

// Destructor joins all threads
ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        worker.join();
}

ThreadPool& ThreadPool::getInstance() {
    static ThreadPool instance([] {
        unsigned hw = std::thread::hardware_concurrency();
        unsigned suggested = (hw > 4) ? hw / 2 : std::max(2u, hw);
        suggested = std::clamp(suggested, 2u, 6u);
        if (const char* env_p = std::getenv("RETROFE_THREADPOOL_SIZE")) {
            unsigned user = std::atoi(env_p);
            if (user > 0 && user < 64)
                suggested = user;
        }
        LOG_INFO("ThreadPool", "Initializing ThreadPool with " + std::to_string(suggested) + " threads (hardware_concurrency=" + std::to_string(hw) + ")");
        return suggested;
        }());
    return instance;
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();
    workers.clear(); // Not strictly necessary but makes state explicit
}

