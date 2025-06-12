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

#include "VideoPool.h"
#include "../Utility/Log.h"
#include <algorithm>
#include <memory>
#include <deque>
#include <unordered_map>

namespace {
	constexpr size_t POOL_BUFFER_INSTANCES = 2;
	constexpr size_t HEALTH_CHECK_ACTIVE_THRESHOLD = 20;
	constexpr int HEALTH_CHECK_INTERVAL = 30;
}

VideoPool::PoolMap VideoPool::pools_;
std::unique_ptr<ThreadPool> VideoPool::threadPool_;

void VideoPool::initializeThreadPool(size_t numThreads) {
	if (!threadPool_) {
		threadPool_ = std::make_unique<ThreadPool>(numThreads);
		LOG_INFO("VideoPool", "ThreadPool initialized with " + std::to_string(numThreads) + " threads");
	}
}

void VideoPool::shutdownThreadPool() {
	threadPool_.reset();
	LOG_INFO("VideoPool", "ThreadPool shutdown");
}

VideoPool::PoolInfo& VideoPool::getPoolInfo(int monitor, int listId) {
	return pools_[monitor][listId]; // will auto-create if not found
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
	if (listId == -1) {
		LOG_DEBUG("VideoPool", "Creating non-pooled instance (listId = -1). Monitor: " + std::to_string(monitor));
		auto instance = std::make_unique<GStreamerVideo>(monitor);
		instance->setSoftOverlay(softOverlay);
		return instance;
	}

	static int callCounter = 0;
	if (++callCounter % HEALTH_CHECK_INTERVAL == (HEALTH_CHECK_INTERVAL - 1)) {
		if (!checkPoolHealth(monitor, listId)) {
			cleanup(monitor, listId);
		}
	}

	PoolInfo& pool = getPoolInfo(monitor, listId);

	VideoPtr vid;
	bool createdNew = false;

	{
		std::lock_guard<std::mutex> lock(pool.poolMutex);

		size_t currentActive = pool.currentActive;
		size_t pooledCount = pool.instances.size();
		size_t currentTotalPopulation = currentActive + pooledCount;

		size_t targetPopulationCapacity = 0;
		if (pool.initialCountLatched) {
			targetPopulationCapacity = pool.requiredInstanceCount + POOL_BUFFER_INSTANCES;
		}
		else {
			targetPopulationCapacity = pool.observedMaxActive + POOL_BUFFER_INSTANCES;
			if (targetPopulationCapacity < POOL_BUFFER_INSTANCES) targetPopulationCapacity = POOL_BUFFER_INSTANCES;
			else if (targetPopulationCapacity == 0) targetPopulationCapacity = 1;
		}

		if (currentTotalPopulation < targetPopulationCapacity) {
			LOG_DEBUG("VideoPool", "Acquire: Population (" + std::to_string(currentTotalPopulation) +
				") < target (" + std::to_string(targetPopulationCapacity) + "), creating new.");
			pool.currentActive++;
			createdNew = true;
		}
		else if (!pool.instances.empty()) {
			LOG_DEBUG("VideoPool", "Acquire: Target met. Reusing from pool.");
			vid = std::move(pool.instances.front());
			pool.instances.pop_front();
			pool.currentActive++;
		}
		else {
			LOG_DEBUG("VideoPool", "Acquire: Target met but pool empty. Creating new as fallback.");
			pool.currentActive++;
			createdNew = true;
		}

		// Update observed max active before latching
		if (!pool.initialCountLatched) {
			if (pool.currentActive > pool.observedMaxActive) {
				pool.observedMaxActive = pool.currentActive;
			}
		}
	} // unlock here

	// Only construct outside the lock!
	if (createdNew) {
		auto gstreamerVid = std::make_unique<GStreamerVideo>(monitor);
		if (gstreamerVid && gstreamerVid->hasError()) {
			LOG_ERROR("VideoPool", "Newly created GStreamerVideo instance has an error. Will be discarded on release.");
		}
		if (gstreamerVid)
			gstreamerVid->setSoftOverlay(softOverlay);
		vid = std::move(gstreamerVid);
	}

	if (vid) {
		// If vid is not null, set the overlay (already done above for new, but ok to repeat)
		if (auto* gvid = dynamic_cast<GStreamerVideo*>(vid.get()))
			gvid->setSoftOverlay(softOverlay);
	}
	else if (!createdNew) {
		LOG_ERROR("VideoPool", "Internal error: vid is null but was not newly created. Should not happen.");
		return nullptr;
	}
	else {
		LOG_ERROR("VideoPool", "Failed to construct new GStreamerVideo instance.");
		return nullptr;
	}

	return vid;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
	if (!vid || listId == -1) return;

	// Lazily initialize threadPool if needed (thread-safe: single-threaded init is fine here)
	if (!threadPool_) {
		initializeThreadPool(3);
	}

	// Move the pointer so the lambda owns it (capture by move)
	auto vidForThread = std::move(vid);

	threadPool_->enqueue([vidPtr = std::move(vidForThread), monitor, listId]() mutable {
		bool isFaulty = false;
		try {
			vidPtr->unload();
			isFaulty = vidPtr->hasError();
		}
		catch (const std::exception& e) {
			LOG_ERROR("VideoPool", "Exception during video unload: " + std::string(e.what()));
			isFaulty = true;
		}
		catch (...) {
			LOG_ERROR("VideoPool", "Unknown exception during video unload.");
			isFaulty = true;
		}

		bool shouldErasePool = false; // <-- for erasure outside the mutex

		// --- Safe reacquire of pool ---
		auto monitorIt = pools_.find(monitor);
		if (monitorIt == pools_.end()) {
			LOG_DEBUG("VideoPool", "releaseVideo: monitor " + std::to_string(monitor) + " no longer exists. Video will be destroyed.");
			return;
		}
		auto listIt = monitorIt->second.find(listId);
		if (listIt == monitorIt->second.end()) {
			LOG_DEBUG("VideoPool", "releaseVideo: listId " + std::to_string(listId) + " for monitor " + std::to_string(monitor) + " no longer exists. Video will be destroyed.");
			return;
		}

		PoolInfo& pool = listIt->second;
		{
			std::lock_guard<std::mutex> lock(pool.poolMutex);

			if (pool.currentActive == 0) {
				LOG_WARNING("VideoPool", "releaseVideo: currentActive already zero on release! Monitor: " +
					std::to_string(monitor) + " List: " + std::to_string(listId));
			}
			else {
				pool.currentActive--;
			}

			if (isFaulty) {
				LOG_DEBUG("VideoPool", "Discarded faulty instance. Active: " + std::to_string(pool.currentActive));
			}
			else {
				pool.instances.push_back(std::move(vidPtr));
				LOG_DEBUG("VideoPool", "Returned instance to pool. Active: " +
					std::to_string(pool.currentActive) + ", Pooled: " + std::to_string(pool.instances.size()));

				if (!pool.initialCountLatched) {
					size_t requiredCount = std::max(pool.observedMaxActive, size_t(1));
					pool.requiredInstanceCount = requiredCount;
					pool.initialCountLatched = true;
					LOG_INFO("VideoPool", "Latched count for Monitor: " + std::to_string(monitor) +
						", List ID: " + std::to_string(listId) +
						". Peak (N): " + std::to_string(requiredCount) +
						". Target: N + " + std::to_string(POOL_BUFFER_INSTANCES));
				}
			}
			if (pool.markedForCleanup && pool.currentActive == 0) {
				shouldErasePool = true; // <-- set flag!
			}
		} // poolMutex UNLOCKED HERE

		// Only erase after lock released
		if (shouldErasePool) {
			auto monitorIt = pools_.find(monitor);
			if (monitorIt != pools_.end()) {
				auto listIt = monitorIt->second.find(listId);
				if (listIt != monitorIt->second.end()) {
					LOG_INFO("VideoPool", "Final cleanup: erasing pool for Monitor: " +
						std::to_string(monitor) + ", List ID: " + std::to_string(listId));
					monitorIt->second.erase(listIt);
					if (monitorIt->second.empty()) {
						pools_.erase(monitorIt);
					}
				}
			}
		}
		});
}

void VideoPool::cleanup(int monitor, int listId) {
	auto monitorIt = pools_.find(monitor);
	if (monitorIt == pools_.end()) return;
	auto listIt = monitorIt->second.find(listId);
	if (listIt == monitorIt->second.end()) return;

	PoolInfo& pool = listIt->second;

	{
		std::lock_guard<std::mutex> lock(pool.poolMutex);
		pool.markedForCleanup = true; // Mark, but don't erase yet
		// Optionally, clear instances now to free up memory
		pool.instances.clear();
	}
	LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
	if (threadPool_) threadPool_.reset();  // <--- Will join all threads, finish all queued tasks
	LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
	for (auto& [monitor, listMap] : pools_) {
		for (auto& [listId, pool] : listMap) {
			LOG_DEBUG("VideoPool", "Clearing pool for Monitor: " + std::to_string(monitor) +
				", List ID: " + std::to_string(listId));
			pool.instances.clear();
			pool.currentActive = 0;
			pool.observedMaxActive = 0;
			pool.initialCountLatched = false;
			pool.requiredInstanceCount = 0;
		}
	}
	pools_.clear();
	LOG_INFO("VideoPool", "VideoPool shutdown complete.");
}

bool VideoPool::checkPoolHealth(int monitor, int listId) {
	auto monitorIt = pools_.find(monitor);
	if (monitorIt == pools_.end()) return true;
	auto listIt = monitorIt->second.find(listId);
	if (listIt == monitorIt->second.end()) return true;

	PoolInfo& pool = listIt->second;
	if (pool.currentActive > HEALTH_CHECK_ACTIVE_THRESHOLD) {
		LOG_WARNING("VideoPool", "Health check: suspicious active count: " + std::to_string(pool.currentActive) +
			" for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		return false;
	}
	return true;
}