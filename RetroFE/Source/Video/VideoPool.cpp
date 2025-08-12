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
	if (listId == -1) { /* ... same as before ... */ }

	PoolInfo* poolPtr = nullptr;
	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		poolPtr = &pools_[monitor][listId];
	}

	VideoPtr vid;
	bool shouldCreateNew = false;

	// This is the main decision loop.
	for (;;) {
		{ // Start of mutex-protected scope
			std::lock_guard<std::mutex> lock(poolPtr->poolMutex);

			// --- ALL DECISION LOGIC IS INSIDE THE LOCK ---
			// Dynamic sizing phase (unchanged from your working code)
			if (!poolPtr->initialCountLatched) {
				poolPtr->currentActive++;
				if (poolPtr->currentActive > poolPtr->observedMaxActive) {
					poolPtr->observedMaxActive = poolPtr->currentActive;
				}
				shouldCreateNew = true;
				break; // Exit loop to create new video
			}

			// PRIORITY 1: Reuse from 'ready' queue.
			if (!poolPtr->ready.empty()) {
				vid = std::move(poolPtr->ready.front());
				poolPtr->ready.pop_front();
				poolPtr->currentActive++;
				break; // Success, exit loop
			}

			// PRIORITY 2: Reclaim from 'pending' queue.
			auto it = poolPtr->pending.begin();
			bool foundInPending = false;
			while (it != poolPtr->pending.end()) {
				if ((*it)->getActualState() == IVideo::VideoState::None) {
					vid = std::move(*it);
					poolPtr->pending.erase(it);
					poolPtr->currentActive++;
					foundInPending = true;
					break;
				}
				++it;
			}
			if (foundInPending) {
				break; // Success, exit loop
			}

			// PRIORITY 3: Create a new instance if the pool is not yet full.
			size_t totalPopulation = poolPtr->currentActive + poolPtr->ready.size() + poolPtr->pending.size();
			if (totalPopulation < poolPtr->requiredInstanceCount) {
				poolPtr->currentActive++;
				shouldCreateNew = true;
				break; // Success, exit loop
			}
		} // Mutex is released here

		// --- THE ACTIVE WAIT ---
		// If we've reached this point, the pool is full and nothing is ready.
		// We now actively process GStreamer events until a video becomes available.
		LOG_DEBUG("VideoPool", "Pool full, actively waiting for an instance to become ready...");

		// Process GStreamer bus messages. This is the critical step that allows
		// videos in 'pending' to change their state to 'None'.
		while (g_main_context_pending(nullptr)) {
			g_main_context_iteration(nullptr, false);
		}

		if (shuttingDown_) return nullptr;
		// The for(;;) will now loop, re-lock, and re-check the queues.
	} // End of for(;;) loop

	if (shouldCreateNew) {
		vid = std::make_unique<GStreamerVideo>(monitor);
		if (!vid || vid->hasError()) {
			LOG_ERROR("VideoPool", "Failed to construct a new GStreamerVideo instance.");
			{
				std::lock_guard<std::mutex> lock(poolPtr->poolMutex);
				if (poolPtr->currentActive > 0) poolPtr->currentActive--;
			}
			return nullptr;
		}
	}

	if (vid) {
		vid->setSoftOverlay(softOverlay);
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

	vid->unload(); // Call the fast, non-blocking unload directly.

	bool latchedOnThisRelease = false;
	size_t latchedPoolSize = 0;

	{
		std::lock_guard<std::mutex> lock(poolPtr->poolMutex);
		if (poolPtr->currentActive > 0) {
			poolPtr->currentActive--;
		}

		poolPtr->pending.push_back(std::move(vid));

		// This is the latching logic, moved here from the worker thread.
		if (!poolPtr->initialCountLatched) {
			poolPtr->requiredInstanceCount = poolPtr->observedMaxActive + POOL_BUFFER_INSTANCES;
			poolPtr->initialCountLatched = true;
			latchedOnThisRelease = true;
			latchedPoolSize = poolPtr->requiredInstanceCount;
		}

		LOG_DEBUG("VideoPool", "Instance moved to pending queue. Active: " +
			std::to_string(poolPtr->currentActive) + ", Pending: " +
			std::to_string(poolPtr->pending.size()) +
			(latchedOnThisRelease ? (" [Latched pool size: " + std::to_string(latchedPoolSize) + "]") : ""));

		poolPtr->poolCond.notify_one();
	}
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
	if (videos.empty() || listId == -1 || shuttingDown_) return;

	for (auto& vid : videos) {
		releaseVideo(std::move(vid), monitor, listId);
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