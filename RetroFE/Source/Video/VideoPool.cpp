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

std::atomic<bool> VideoPool::shuttingDown_ = false;

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

	PoolInfo* poolPtr = nullptr;
	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		poolPtr = &pools_[monitor][listId];
	}

	VideoPtr vid;
	bool shouldCreateNew = false;

	{
		// Acquire the local pool mutex after global lock is released
		// This reduces contention on the global lock for the bulk of acquire logic.
		std::unique_lock<std::mutex> lock(poolPtr->poolMutex);

		if (!poolPtr->initialCountLatched) {
			poolPtr->currentActive++;
			if (poolPtr->currentActive > poolPtr->observedMaxActive) {
				poolPtr->observedMaxActive = poolPtr->currentActive;
			}
			size_t totalPopulation = poolPtr->currentActive + poolPtr->ready.size() + poolPtr->pending.size();
			size_t futurePoolSize = poolPtr->observedMaxActive + POOL_BUFFER_INSTANCES;
			LOG_DEBUG("VideoPool", "Acquire: Dynamic phase. Population (" +
				std::to_string(totalPopulation) + "), will latch size: " +
				std::to_string(futurePoolSize) + " after first release.");
			shouldCreateNew = true;
		}
		else {
			size_t totalPopulation = poolPtr->currentActive + poolPtr->ready.size() + poolPtr->pending.size();
			size_t poolSize = poolPtr->requiredInstanceCount;

			if (totalPopulation < poolSize) {
				poolPtr->currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Pool not full (population: " +
					std::to_string(totalPopulation) + "/" + std::to_string(poolSize) +
					"), creating new.");
				shouldCreateNew = true;
			}
			else if (!poolPtr->ready.empty()) {
				vid = std::move(poolPtr->ready.front());
				poolPtr->ready.pop_front();
				poolPtr->currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Reusing from ready pool. "
					"Active: " + std::to_string(poolPtr->currentActive) +
					", Ready: " + std::to_string(poolPtr->ready.size()) +
					", Pending: " + std::to_string(poolPtr->pending.size()));
			}
			else {
				LOG_DEBUG("VideoPool", "Acquire: Pool full and all instances busy. Waiting for ready.");
				// This wait needs to be outside the global lock. It is, so that's good.
				poolPtr->poolCond.wait(lock, [poolPtr] { return !poolPtr->ready.empty() || shuttingDown_; });
				if (shuttingDown_) { // Check for shutdown while waiting
					return nullptr; // Or throw, or handle appropriately
				}
				vid = std::move(poolPtr->ready.front());
				poolPtr->ready.pop_front();
				poolPtr->currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Acquired after wait. "
					"Active: " + std::to_string(poolPtr->currentActive) +
					", Ready: " + std::to_string(poolPtr->ready.size()) +
					", Pending: " + std::to_string(poolPtr->pending.size()));
			}
		}
	} // poolMutex released

	if (shouldCreateNew) {
		auto gstreamerVid = std::make_unique<GStreamerVideo>(monitor);
		if (!gstreamerVid) {
			LOG_ERROR("VideoPool", "Failed to construct new GStreamerVideo instance.");
			// If creation fails, we might need to decrement currentActive if it was incremented.
			// This is tricky: if currentActive was incremented, but no vid is returned,
			// it leads to an inflated currentActive count.
			// Consider rolling back currentActive or handling failure more robustly.
			// For now, it will be cleaned up if it causes a problem.
			return nullptr;
		}
		if (gstreamerVid->hasError()) {
			LOG_ERROR("VideoPool", "Newly created GStreamerVideo instance has an error. Will be discarded on release.");
		}
		gstreamerVid->setSoftOverlay(softOverlay);
		vid = std::move(gstreamerVid);
	}

	if (vid) {
		if (auto* gvid = dynamic_cast<GStreamerVideo*>(vid.get())) {
			gvid->setSoftOverlay(softOverlay);
		}
	}

	if (!vid) {
		LOG_ERROR("VideoPool", "Internal error: acquireVideo failed unexpectedly.");
	}
	return vid;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
	if (!vid || listId == -1 || shuttingDown_) return;

	PoolInfo* poolPtr = nullptr;
	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		auto monitorIt = pools_.find(monitor);
		if (monitorIt == pools_.end()) return;
		auto listIt = monitorIt->second.find(listId);
		if (listIt == monitorIt->second.end()) return;
		poolPtr = &listIt->second;
	}

	bool latchedOnThisRelease = false;
	size_t latchedPoolSize = 0;

	{
		std::lock_guard<std::mutex> lock(poolPtr->poolMutex);
		if (poolPtr->currentActive > 0) {
			poolPtr->currentActive--;
		}
		else {
			LOG_WARNING("VideoPool", "releaseVideo called but currentActive was already zero. "
				"Monitor: " + std::to_string(monitor) + ", List: " + std::to_string(listId));
		}

		poolPtr->pending.push_back(std::move(vid));

		if (!poolPtr->initialCountLatched) {
			poolPtr->requiredInstanceCount = poolPtr->observedMaxActive + POOL_BUFFER_INSTANCES;
			poolPtr->initialCountLatched = true;
			latchedOnThisRelease = true;
			latchedPoolSize = poolPtr->requiredInstanceCount;
		}
	}

	LOG_DEBUG("VideoPool", "Instance moved to pending queue. Active: " +
		std::to_string(poolPtr->currentActive) + ", Pending: " +
		std::to_string(poolPtr->pending.size()) +
		(latchedOnThisRelease ? (" [Latched pool size: " + std::to_string(latchedPoolSize) + "]") : ""));

	// Now start async worker
	ThreadPool::getInstance().enqueue([monitor, listId]() {
		VideoPtr videoToProcess;
		bool poolIsMarkedForCleanup = false; // *** MODIFICATION ***

		// --- Phase 1: Dequeue a video and check cleanup status ---
		{
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);
			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) return;
			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) return;

			PoolInfo& pool = listIt->second;
			std::lock_guard<std::mutex> localLock(pool.poolMutex);
			if (pool.pending.empty()) return;

			poolIsMarkedForCleanup = pool.markedForCleanup; // *** MODIFICATION ***
			videoToProcess = std::move(pool.pending.front());
			pool.pending.pop_front();
		}

		// *** MODIFICATION START ***
		// If the pool was marked for cleanup when we took the video, just let the
		// video instance be destroyed without performing a wasteful unload.
		if (poolIsMarkedForCleanup) {
			LOG_DEBUG("VideoPool_Worker", "Skipping unload for cleanup-marked pool. Instance will be destroyed. Monitor: " + std::to_string(monitor) + ", List: " + std::to_string(listId));
			return; // The VideoPtr goes out of scope, calling the destructor which calls stop().
		}
		// *** MODIFICATION END ***

		// --- Phase 2: Perform the long-running work (only if not cleaning up) ---
		bool isFaulty = false;
		try {
			if (videoToProcess) {
				videoToProcess->unload();
				isFaulty = videoToProcess->hasError();
			}
		}
		catch (...) {
			LOG_ERROR("VideoPool_Worker", "Exception during video unload.");
			isFaulty = true;
		}

		// --- Phase 3: Re-integrate the video and check for cleanup ---
		{
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);
			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) return;

			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) return;

			PoolInfo& pool = listIt->second;
			std::lock_guard<std::mutex> localLock(pool.poolMutex);

			if (pool.markedForCleanup && pool.currentActive == 0 && pool.pending.empty() && pool.ready.empty()) {
				LOG_INFO("VideoPool_Worker", "Performing final cleanup for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
				monitorIt->second.erase(listIt);
				if (monitorIt->second.empty()) {
					pools_.erase(monitorIt);
				}
				return;
			}

			// The check for `pool.markedForCleanup` here is still necessary in case the pool
			// was marked for cleanup *while* unload() was running.
			if (!isFaulty && videoToProcess && !pool.markedForCleanup) {
				pool.ready.push_back(std::move(videoToProcess));
				pool.poolCond.notify_one();
			}
			else {
				// Discard faulty or cleanup-marked videos
				LOG_DEBUG("VideoPool_Worker", "Discarding video instance post-unload. Faulty: " + std::string(isFaulty ? "yes" : "no") + ", Marked for cleanup: " + std::string(pool.markedForCleanup ? "yes" : "no"));
			}
		}
		});
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
	if (videos.empty() || listId == -1 || shuttingDown_) return;

	size_t releaseCount = videos.size();
	PoolInfo* poolPtr = nullptr;

	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		auto monitorIt = pools_.find(monitor);
		if (monitorIt == pools_.end()) {
			LOG_WARNING("VideoPool", "releaseVideoBatch: No pool found for monitor=" + std::to_string(monitor));
			return;
		}
		auto listIt = monitorIt->second.find(listId);
		if (listIt == monitorIt->second.end()) {
			LOG_WARNING("VideoPool", "releaseVideoBatch: No pool found for listId=" + std::to_string(listId));
			return;
		}
		poolPtr = &listIt->second;
	}

	bool latchedOnThisRelease = false;
	size_t latchedPoolSize = 0;

	{
		std::lock_guard<std::mutex> lock(poolPtr->poolMutex);

		poolPtr->currentActive = (poolPtr->currentActive > releaseCount)
			? poolPtr->currentActive - releaseCount
			: 0;

		for (auto& vid : videos)
			poolPtr->pending.push_back(std::move(vid));

		if (!poolPtr->initialCountLatched) {
			poolPtr->requiredInstanceCount = poolPtr->observedMaxActive + POOL_BUFFER_INSTANCES;
			poolPtr->initialCountLatched = true;
			latchedOnThisRelease = true;
			latchedPoolSize = poolPtr->requiredInstanceCount;
		}
	}

	LOG_DEBUG("VideoPool", "Batch release: " + std::to_string(releaseCount) +
		" videos queued. Active now: " + std::to_string(poolPtr->currentActive) +
		", Pending: " + std::to_string(poolPtr->pending.size()) +
		(latchedOnThisRelease ? (" [Latched pool size: " + std::to_string(latchedPoolSize) + "]") : ""));

	for (size_t i = 0; i < releaseCount; ++i) {
		ThreadPool::getInstance().enqueue([monitor, listId]() {
			VideoPtr videoToProcess;
			bool poolIsMarkedForCleanup = false; // *** MODIFICATION ***

			{
				std::lock_guard<std::mutex> globalLock(s_poolsMutex);
				auto monitorIt = pools_.find(monitor);
				if (monitorIt == pools_.end()) return;
				auto listIt = monitorIt->second.find(listId);
				if (listIt == monitorIt->second.end()) return;

				PoolInfo& pool = listIt->second;
				std::lock_guard<std::mutex> localLock(pool.poolMutex);
				if (pool.pending.empty()) return;

				poolIsMarkedForCleanup = pool.markedForCleanup; // *** MODIFICATION ***
				videoToProcess = std::move(pool.pending.front());
				pool.pending.pop_front();
			}

			// *** MODIFICATION START ***
			if (poolIsMarkedForCleanup) {
				return; // Efficiently discard
			}
			// *** MODIFICATION END ***

			bool isFaulty = false;
			try {
				if (videoToProcess) {
					videoToProcess->unload();
					isFaulty = videoToProcess->hasError();
				}
			}
			catch (...) {
				LOG_ERROR("VideoPool_Worker", "Exception during video unload.");
				isFaulty = true;
			}

			{
				std::lock_guard<std::mutex> globalLock(s_poolsMutex);
				auto monitorIt = pools_.find(monitor);
				if (monitorIt == pools_.end()) return;
				auto listIt = monitorIt->second.find(listId);
				if (listIt == monitorIt->second.end()) return;

				PoolInfo& pool = listIt->second;
				std::lock_guard<std::mutex> localLock(pool.poolMutex);

				if (pool.markedForCleanup && pool.currentActive == 0 &&
					pool.pending.empty() && pool.ready.empty()) {
					LOG_INFO("VideoPool_Worker", "Final cleanup after batch release.");
					monitorIt->second.erase(listIt);
					if (monitorIt->second.empty()) {
						pools_.erase(monitorIt);
					}
					return;
				}

				if (!isFaulty && videoToProcess && !pool.markedForCleanup) {
					pool.ready.push_back(std::move(videoToProcess));
					pool.poolCond.notify_one();
				}
				else {
					LOG_DEBUG("VideoPool_Worker", "Discarding faulty or cleanup video.");
					pool.poolCond.notify_one();
				}
			}
			});
	}
}


void VideoPool::cleanup_nolock(int monitor, int listId) {
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
	shuttingDown_ = true;

	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		for (auto& [monitor, listMap] : pools_) {
			for (auto& [listId, pool] : listMap) {
				std::lock_guard<std::mutex> localLock(pool.poolMutex);
				pool.markedForCleanup = true;
				pool.poolCond.notify_all(); // Unblock any waiting threads
			}
		}
	}

	// Wait for all workers to complete
	ThreadPool::getInstance().wait();

	// Now safely clear all pools
	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		pools_.clear();
	}

	shuttingDown_ = false;
	LOG_INFO("VideoPool", "VideoPool shutdown complete.");
}

bool VideoPool::checkPoolHealth(int monitor, int listId) {
	// globalLock is already held by acquireVideo when this is called.
	auto monitorIt = pools_.find(monitor);
	if (monitorIt == pools_.end()) return true;
	auto listIt = monitorIt->second.find(listId);
	if (listIt == monitorIt->second.end()) return true;

	PoolInfo& pool = listIt->second;
	{
		// FIX: Acquire poolMutex to safely read currentActive
		std::lock_guard<std::mutex> lock(pool.poolMutex);
		if (pool.currentActive > HEALTH_CHECK_ACTIVE_THRESHOLD) {
			LOG_WARNING("VideoPool", "Health check: suspicious active count: " + std::to_string(pool.currentActive) +
				" for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			return false;
		}
	} // lock released
	return true;
}