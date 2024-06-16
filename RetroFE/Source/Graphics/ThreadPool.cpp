#include "ThreadPool.h"
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
void pinThreadToCore(std::thread& th, DWORD_PTR core_id)
{
    HANDLE handle = th.native_handle();
    DWORD_PTR affinity_mask = 1ULL << core_id; // Ensure the shift operation is on a 64-bit value
    SetThreadAffinityMask(handle, affinity_mask);
}
#else
#include <pthread.h>
void pinThreadToCore(std::thread& th, int core_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
}
#endif

// Constructor
ThreadPool::ThreadPool(size_t threads) : stop(false)
{
    for (size_t i = 0; i < threads; ++i)
    {
        workers.emplace_back([this] {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });

                    if (this->stop && this->tasks.empty())
                        return;

                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }

                try
                {
                    task();
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Task threw an exception: " << e.what() << std::endl;
                }
                catch (...)
                {
                    std::cerr << "Task threw an unknown exception." << std::endl;
                }
            }
            });

        // Pin thread to a core
#ifdef _WIN32
        pinThreadToCore(workers.back(), i % std::thread::hardware_concurrency());
#else
        pinThreadToCore(workers.back(), i % std::thread::hardware_concurrency());
#endif
    }
}

// Destructor joins all threads
ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        worker.join();
}