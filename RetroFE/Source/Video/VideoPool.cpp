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

	PoolInfo* poolPtr = nullptr;
	{
		std::lock_guard<std::mutex> globalLock(s_poolsMutex);
		static int callCounter = 0;
		if (++callCounter % HEALTH_CHECK_INTERVAL == (HEALTH_CHECK_INTERVAL - 1)) {
			if (!checkPoolHealth(monitor, listId)) {
				cleanup(monitor, listId);
			}
		}
		poolPtr = &pools_[monitor][listId];
	}

	VideoPtr vid;
	bool shouldCreateNew = false;
	bool dynamicPhase = false;

	{
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
			dynamicPhase = true;
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
				poolPtr->poolCond.wait(lock, [poolPtr] { return !poolPtr->ready.empty(); });
				vid = std::move(poolPtr->ready.front());
				poolPtr->ready.pop_front();
				poolPtr->currentActive++;
				LOG_DEBUG("VideoPool", "Acquire: Acquired after wait. "
					"Active: " + std::to_string(poolPtr->currentActive) +
					", Ready: " + std::to_string(poolPtr->ready.size()) +
					", Pending: " + std::to_string(poolPtr->pending.size()));
			}
		}
	}

	if (shouldCreateNew) {
		auto gstreamerVid = std::make_unique<GStreamerVideo>(monitor);
		if (!gstreamerVid) {
			LOG_ERROR("VideoPool", "Failed to construct new GStreamerVideo instance.");
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
	if (!vid || listId == -1) return;

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

		LOG_DEBUG("VideoPool", "Instance moved to pending queue. Active: " +
			std::to_string(poolPtr->currentActive) + ", Pending: " +
			std::to_string(poolPtr->pending.size()) +
			(latchedOnThisRelease ? (" [Latched pool size: " + std::to_string(latchedPoolSize) + "]") : ""));
	}

	// Now start async worker
	ThreadPool::getInstance().enqueue([monitor, listId]() {
		VideoPtr videoToProcess;

		PoolInfo* poolPtr = nullptr;
		{
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);
			auto monitorIt = pools_.find(monitor);
			if (monitorIt == pools_.end()) return;
			auto listIt = monitorIt->second.find(listId);
			if (listIt == monitorIt->second.end()) return;
			poolPtr = &listIt->second;
		}

		{
			std::lock_guard<std::mutex> localLock(poolPtr->poolMutex);
			if (poolPtr->pending.empty()) return;
			videoToProcess = std::move(poolPtr->pending.front());
			poolPtr->pending.pop_front();
		}

		bool isFaulty = false;
		try {
			videoToProcess->unload();
			isFaulty = videoToProcess->hasError();
		}
		catch (...) {
			LOG_ERROR("VideoPool_Worker", "Exception during unload.");
			isFaulty = true;
		}

		bool doFinalCleanup = false;
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
				pool.poolCond.notify_one();
			}

			if (pool.markedForCleanup && !pool.cleanupInProgress &&
				pool.currentActive == 0 && pool.pending.empty()) {
				pool.cleanupInProgress = true;
				doFinalCleanup = true;
			}
		}

		if (doFinalCleanup) {
			std::lock_guard<std::mutex> globalLock(s_poolsMutex);
			auto monitorIt = pools_.find(monitor);
			if (monitorIt != pools_.end()) {
				monitorIt->second.erase(listId);
				if (monitorIt->second.empty()) {
					pools_.erase(monitorIt);
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