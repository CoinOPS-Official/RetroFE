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
#include "../Utility/ThreadPool.h"
#include <algorithm>
#include <memory>
#include <deque>
#include <unordered_map>

std::mutex VideoPool::s_poolsMutex;

namespace {
	constexpr size_t POOL_BUFFER_INSTANCES = 2;
	constexpr size_t HEALTH_CHECK_ACTIVE_THRESHOLD = 20;
	constexpr int HEALTH_CHECK_INTERVAL = 30;
}

VideoPool::PoolMap VideoPool::pools_;

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

	// Only lock globally for map lookup/health
	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		static int callCounter = 0;
		if (++callCounter % HEALTH_CHECK_INTERVAL == (HEALTH_CHECK_INTERVAL - 1)) {
			if (!checkPoolHealth(monitor, listId)) {
				cleanup(monitor, listId);
			}
		}
	}

	PoolInfo& pool = getPoolInfo(monitor, listId);

	VideoPtr vid;
	bool createdNew = false;
	size_t totalPopulation = 0;
	size_t poolSize = 0;

	{
		std::unique_lock<std::mutex> lock(pool.poolMutex);

		// --- Dynamic phase (pool size not yet latched) ---
		if (!pool.initialCountLatched) {
			pool.currentActive++;
			if (pool.currentActive > pool.observedMaxActive) {
				pool.observedMaxActive = pool.currentActive;
			}
			totalPopulation = pool.currentActive + pool.ready.size() + pool.pending.size();
			poolSize = pool.observedMaxActive + POOL_BUFFER_INSTANCES;
			LOG_DEBUG("VideoPool", "Acquire: Dynamic phase. Population (" +
				std::to_string(totalPopulation) + "), will latch size: " +
				std::to_string(poolSize) + " after first release.");
			createdNew = true; // Always create new until latch
			// unlock before construction
			lock.unlock();
		}
		// --- Fixed-size phase (pool size latched after first release) ---
		else {
			totalPopulation = pool.currentActive + pool.ready.size() + pool.pending.size();
			poolSize = pool.requiredInstanceCount; // already includes buffer

			// If the pool is not yet full (can happen right after latch), create more
			if (totalPopulation < poolSize) {
				pool.currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Pool not full (population: " +
					std::to_string(totalPopulation) + "/" + std::to_string(poolSize) +
					"), creating new.");
				createdNew = true;
				lock.unlock();
			}
			// If ready, grab from ready queue
			else if (!pool.ready.empty()) {
				vid = std::move(pool.ready.front());
				pool.ready.pop_front();
				pool.currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Reusing from ready pool. "
					"Active: " + std::to_string(pool.currentActive) +
					", Ready: " + std::to_string(pool.ready.size()) +
					", Pending: " + std::to_string(pool.pending.size()));
			}
			// Else, block until a video is ready
			else {
				LOG_DEBUG("VideoPool", "Acquire: Pool full and all instances busy. Waiting for ready.");
				pool.poolCond.wait(lock, [&pool] { return !pool.ready.empty(); });
				vid = std::move(pool.ready.front());
				pool.ready.pop_front();
				pool.currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Acquired after wait. "
					"Active: " + std::to_string(pool.currentActive) +
					", Ready: " + std::to_string(pool.ready.size()) +
					", Pending: " + std::to_string(pool.pending.size()));
			}
		}
	}

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
	if (!vid || listId == -1) {
		return; // Nothing to do for null or non-pooled instances
	}

	PoolInfo& pool = getPoolInfo(monitor, listId);

	bool latchedOnThisRelease = false;
	size_t latchedPoolSize = 0;

	{
		std::lock_guard<std::mutex> lock(pool.poolMutex);

		// Decrement active, log state
		if (pool.currentActive > 0) {
			pool.currentActive--;
		}
		else {
			LOG_WARNING("VideoPool", "releaseVideo called but currentActive was already zero. "
				"Monitor: " + std::to_string(monitor) + ", List: " + std::to_string(listId));
		}

		// Move to pending queue
		pool.pending.push_back(std::move(vid));

		// Latch the pool size if this is the first release
		if (!pool.initialCountLatched) {
			pool.requiredInstanceCount = pool.observedMaxActive + POOL_BUFFER_INSTANCES;
			pool.initialCountLatched = true;
			latchedOnThisRelease = true;
			latchedPoolSize = pool.requiredInstanceCount;
		}

		LOG_DEBUG("VideoPool", "Instance moved to pending queue. Active: " +
			std::to_string(pool.currentActive) + ", Pending: " +
			std::to_string(pool.pending.size()) +
			(latchedOnThisRelease ? (" [Latched pool size: " + std::to_string(latchedPoolSize) + "]") : ""));
	}

	// --- Stage 2: Enqueue the Asynchronous Work (Worker Thread) ---
	ThreadPool::getInstance().enqueue([monitor, listId]() mutable {
		VideoPtr videoToProcess;

		// --- Phase 1: Acquire Work Item (Safely) ---
		{
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);

			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) return;
			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) return;

			PoolInfo& pool = listIt->second;
			std::lock_guard<std::mutex> localLock(pool.poolMutex);

			if (pool.pending.empty()) return;

			videoToProcess = std::move(pool.pending.front());
			pool.pending.pop_front();
		} // Unlocks

		// --- Phase 2: Slow I/O (No Locks Held) ---
		bool isFaulty = false;
		try {
			videoToProcess->unload();
			isFaulty = videoToProcess->hasError();
		}
		catch (const std::exception& e) {
			LOG_ERROR("VideoPool_Worker", "Exception during video unload: " + std::string(e.what()));
			isFaulty = true;
		}
		catch (...) {
			LOG_ERROR("VideoPool_Worker", "Unknown exception during video unload.");
			isFaulty = true;
		}

		// --- Phase 3: Move to ready and notify waiters, or discard faulty ---
		bool amITheCleaner = false;
		{
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);

			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) return;
			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) return;

			PoolInfo& pool = listIt->second;
			std::lock_guard<std::mutex> localLock(pool.poolMutex);

			if (!isFaulty) {
				pool.ready.push_back(std::move(videoToProcess));
				LOG_DEBUG("VideoPool_Worker", "Instance moved to ready queue. Ready: " +
					std::to_string(pool.ready.size()) + ", Pending: " +
					std::to_string(pool.pending.size()) + ", Active: " +
					std::to_string(pool.currentActive));
				pool.poolCond.notify_one(); // Wake any waiting acquires
			}
			else {
				LOG_DEBUG("VideoPool_Worker", "Discarded faulty instance.");
			}

			if (pool.markedForCleanup && !pool.cleanupInProgress &&
				pool.currentActive == 0 && pool.pending.empty()) {
				pool.cleanupInProgress = true;
				amITheCleaner = true;
			}
		}

		// --- Phase 4: Final deletion if responsible ---
		if (amITheCleaner) {
			LOG_INFO("VideoPool_Worker", "Final cleanup: erasing pool for Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);

			auto finalMonitorIt = pools_.find(monitor);
			if (finalMonitorIt != pools_.end()) {
				finalMonitorIt->second.erase(listId);
				if (finalMonitorIt->second.empty()) {
					pools_.erase(finalMonitorIt);
				}
			}
		}
		});
}

void VideoPool::cleanup(int monitor, int listId) {
	std::lock_guard<std::mutex> globalLock(s_poolsMutex);
	auto monitorIt = pools_.find(monitor);
	if (monitorIt == pools_.end()) return;
	auto listIt = monitorIt->second.find(listId);
	if (listIt == monitorIt->second.end()) return;

	PoolInfo& pool = listIt->second;

	{
		std::lock_guard<std::mutex> lock(pool.poolMutex);
		pool.markedForCleanup = true; // Mark, but don't erase yet
	}
	LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
	LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
	std::lock_guard<std::mutex> globalLock(s_poolsMutex);
	for (auto& [monitor, listMap] : pools_) {
		for (auto& [listId, pool] : listMap) {
			LOG_DEBUG("VideoPool", "Clearing pool for Monitor: " + std::to_string(monitor) +
				", List ID: " + std::to_string(listId));
			pool.ready.clear();
			pool.pending.clear();
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