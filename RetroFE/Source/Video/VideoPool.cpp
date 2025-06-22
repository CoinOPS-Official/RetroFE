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

	std::lock_guard<std::mutex> globalLock(s_poolsMutex);

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
		size_t readyCount = pool.ready.size();
		size_t pendingCount = pool.pending.size();
		size_t currentTotalPopulation = currentActive + readyCount + pendingCount;

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
		else if (!pool.ready.empty()) {
			LOG_DEBUG("VideoPool", "Acquire: Target met. Reusing from ready pool.");
			vid = std::move(pool.ready.front());
			pool.ready.pop_front();
			pool.currentActive++;
		}
		else {
			LOG_DEBUG("VideoPool", "Acquire: Target met but ready pool empty. Creating new as fallback.");
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
	if (!vid || listId == -1) {
		return; // Nothing to do for null or non-pooled instances
	}

	std::lock_guard<std::mutex> globalLock(s_poolsMutex);

	PoolInfo& pool = getPoolInfo(monitor, listId);

	// --- Stage 1: Immediate State Transition (Main Thread) ---
	// This is the critical change. We lock the pool, decrement the active
	// count, and add the instance to the pending queue all in one go.
	// The instance is now officially "inactive" from the perspective of acquireVideo.
	{
		std::lock_guard<std::mutex> lock(pool.poolMutex);

		if (pool.currentActive > 0) {
			pool.currentActive--;
		}
		else {
			LOG_WARNING("VideoPool", "releaseVideo called but currentActive was already zero. "
				"Monitor: " + std::to_string(monitor) + ", List: " + std::to_string(listId));
		}

		pool.pending.push_back(std::move(vid));

		LOG_DEBUG("VideoPool", "Instance moved to pending queue. Active: " +
			std::to_string(pool.currentActive) + ", Pending: " +
			std::to_string(pool.pending.size()));
	}

	// --- Stage 2: Enqueue the Asynchronous Work (Worker Thread) ---
	// The worker's job is now simpler. It just processes the queue.
	// It does NOT touch `currentActive`.
// In VideoPool.cpp, inside releaseVideo()

	ThreadPool::getInstance().enqueue([monitor, listId]() mutable {
		VideoPtr videoToProcess;

		// --- Phase 1: Acquire Work Item (Safely) ---
		{
			// 1a. Lock the master mutex to safely access the `pools_` map.
			// This prevents the main thread's `shutdown()` from destroying the map
			// while we are trying to read from it.
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);

			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) {
				// The pool for this monitor was already destroyed. Our work is done.
				return;
			}
			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) {
				// The pool for this specific list was already destroyed. Our work is done.
				return;
			}

			// 1b. Now that we have a valid reference to the pool's data,
			// lock its specific mutex to modify its contents (the pending queue).
			PoolInfo& pool = listIt->second;
			std::lock_guard<std::mutex> localLock(pool.poolMutex);

			if (pool.pending.empty()) {
				// Another worker thread might have grabbed the last item. This is normal.
				return;
			}

			// Take an item from the work queue.
			videoToProcess = std::move(pool.pending.front());
			pool.pending.pop_front();
		} // Both locks (`globalLock` and `localLock`) are released here.


		// --- Phase 2: Perform Slow I/O (No Locks Held) ---
		// The `unload()` operation is expensive and is performed entirely outside
		// of any locks, allowing other threads to continue their work.
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


		// --- Phase 3: Return Result and Check for Cleanup (Safely) ---
		bool amITheCleaner = false;
		{
			// 3a. Re-lock the master mutex to safely find the pool again.
			// It could have been destroyed by `shutdown()` during our `unload()` call.
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);

			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) return; // Pool gone.
			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) return; // Pool gone.

			// 3b. Lock the specific pool's mutex to modify its contents.
			PoolInfo& pool = listIt->second;
			std::lock_guard<std::mutex> localLock(pool.poolMutex);

			if (!isFaulty) {
				// The instance is healthy, return it to the ready queue.
				pool.ready.push_back(std::move(videoToProcess));
			}
			else {
				LOG_DEBUG("VideoPool_Worker", "Discarded faulty instance.");
				// The faulty instance's unique_ptr is destroyed when this block ends.
			}

			// 3c. Atomically check if we are the one responsible for cleanup.
			if (pool.markedForCleanup && !pool.cleanupInProgress &&
				pool.currentActive == 0 && pool.pending.empty())
			{
				// The conditions are met AND no one else has started cleaning up.
				// We claim the job.
				pool.cleanupInProgress = true;
				amITheCleaner = true;
			}
		} // Both locks are released here.


		// --- Phase 4: Perform Final Deletion (If Responsible) ---
		if (amITheCleaner) {
			LOG_INFO("VideoPool_Worker", "Final cleanup: erasing pool for Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId));

			// Re-lock the master mutex one last time to perform the final erase.
			// This is safe from other workers (because of `amITheCleaner`) and
			// safe from the main thread (because of the lock).
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);

			auto finalMonitorIt = pools_.find(monitor);
			if (finalMonitorIt != pools_.end()) {
				finalMonitorIt->second.erase(listId); // Erase the list's map entry.
				if (finalMonitorIt->second.empty()) {
					// If this was the last list for the monitor, erase the monitor's map entry.
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