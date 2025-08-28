#include "ThreadPool.h"
#include "Log.h"
#ifdef __linux__
#include <pthread.h> // for pthread_setname_np
#endif
#include <cstdlib>   // for std::getenv
#include <algorithm> // for std::clamp

// Constructor
ThreadPool::ThreadPool(size_t threads) : stop(false), activeWorkers(0) {
	for (size_t i = 0; i < threads; ++i)
		workers.emplace_back([this] {
#if defined(__linux__)
		pthread_setname_np(pthread_self(), "retrofe:pool");
#endif
		for (;;) {
			std::function<void()> task;

			{
				std::unique_lock<std::mutex> lock(this->queueMutex);
				this->condition.wait(lock, [this] {
					return this->stop || !this->tasks.empty();
					});

				if (this->stop && this->tasks.empty())
					return;

				task = std::move(this->tasks.front());
				this->tasks.pop();
				++activeWorkers;
			}

			// RAII guard ensures activeWorkers is decremented even if task throws
			struct Guard {
				ThreadPool* self;
				~Guard() {
					std::unique_lock<std::mutex> lock(self->queueMutex);
					--self->activeWorkers;
					if (self->tasks.empty() && self->activeWorkers == 0)
						self->waitCondition.notify_all();
				}
			} guard{ this };

			try {
				task();
			}
			catch (const std::exception& e) {
				LOG_ERROR("ThreadPool", std::string("task threw: ") + e.what());
			}
			catch (...) {
				LOG_ERROR("ThreadPool", "task threw unknown exception");
			}
		}
			});
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
		unsigned suggested = hw ? std::min(3u, hw) : 3u; // keep 1+ cores free
		if (const char* env_p = std::getenv("RETROFE_THREADPOOL_SIZE")) {
			unsigned user = std::atoi(env_p);
			if (user > 0 && user < 64) suggested = user;
		}
		LOG_INFO("ThreadPool", "Initializing ThreadPool with " +
			std::to_string(suggested) + " threads (hardware_concurrency=" + std::to_string(hw) + ")");
		return suggested;
		}());
	return instance;
}

void ThreadPool::wait() {
	std::unique_lock<std::mutex> lock(queueMutex);
	waitCondition.wait(lock, [this] { return tasks.empty() && activeWorkers == 0; });
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

