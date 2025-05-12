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
#include "GStreamerVideo.h"
#include "../Utility/Log.h"
#include <chrono>
#include <memory>
#include <string>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

namespace {
	// Keep only relevant constants
	constexpr size_t POOL_BUFFER_INSTANCES = 2; // Default to 1 extra instance
	constexpr int ACQUIRE_MAX_RETRIES = 5;
	constexpr std::chrono::milliseconds ACQUIRE_BASE_BACKOFF{ 20 };
	constexpr std::chrono::milliseconds ACQUIRE_LOCK_TIMEOUT{ 100 };
	constexpr std::chrono::milliseconds ACQUIRE_WAIT_TIMEOUT{ 500 };
	constexpr std::chrono::milliseconds RELEASE_LOCK_TIMEOUT{ 300 };
	constexpr size_t HEALTH_CHECK_ACTIVE_THRESHOLD = 20; // Keep health check
	constexpr int HEALTH_CHECK_INTERVAL = 30; // Keep health check interval
}
// --- End Constants ---

VideoPool::PoolMap VideoPool::pools_;
std::shared_mutex VideoPool::mapMutex_;

VideoPool::PoolInfoPtr VideoPool::getPoolInfo(int monitor, int listId) {
    // Try read-only access first
    {
        std::shared_lock readLock(mapMutex_);
        auto monitorIt = pools_.find(monitor);
        if (monitorIt != pools_.end()) {
            auto listIt = monitorIt->second.find(listId);
            if (listIt != monitorIt->second.end()) {
                return listIt->second; // Return existing shared_ptr
            }
        }
    } // readLock released here

    // Need to create new pool info, acquire write lock
    std::unique_lock writeLock(mapMutex_);
    // Check again after acquiring write lock
    auto monitorIt = pools_.find(monitor);
    if (monitorIt != pools_.end()) {
        auto listIt = monitorIt->second.find(listId);
        if (listIt != monitorIt->second.end()) {
            return listIt->second; // Another thread created it
        }
        // Monitor exists, but listId doesn't
        LOG_DEBUG("VideoPool", "Creating new pool entry (shared_ptr) for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        auto newPoolInfo = std::make_shared<PoolInfo>();
        auto result = monitorIt->second.emplace(listId, newPoolInfo);
        return result.first->second; // Return newly created shared_ptr
    }
    // Neither monitor nor listId exists
    LOG_DEBUG("VideoPool", "Creating new pool entry (shared_ptr) for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    auto newPoolInfo = std::make_shared<PoolInfo>();
    // Use emplace for potentially better efficiency
    auto resultMon = pools_.emplace(monitor, ListPoolMap{});
    auto resultList = resultMon.first->second.emplace(listId, newPoolInfo);
    return resultList.first->second; // Return newly created shared_ptr
}

std::unique_ptr<IVideo> VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
	if (listId == -1) {
		LOG_DEBUG("VideoPool", "Creating non-pooled instance (listId = -1). Monitor: " + std::to_string(monitor));
		auto instance = std::make_unique<GStreamerVideo>(monitor);
		instance->setSoftOverlay(softOverlay);
		return instance;
	}

	// Periodic health check
	static std::atomic<int> callCounter{ 0 };
	if (callCounter.fetch_add(1, std::memory_order_relaxed) % HEALTH_CHECK_INTERVAL == (HEALTH_CHECK_INTERVAL - 1)) {
		if (!checkPoolHealth(monitor, listId)) {
			cleanup(monitor, listId);
		}
	}

	PoolInfoPtr poolInfoPtr = getPoolInfo(monitor, listId);

	// This is the main lock for the specific pool's (listId's) data
	std::unique_lock<std::timed_mutex> pool_instance_lock; // Renamed for clarity from 'poolLock' to avoid confusion if I use 'poolLock' as a generic term
	for (int retries = 0; ; ++retries) {
		if (poolInfoPtr->poolMutex.try_lock_for(ACQUIRE_LOCK_TIMEOUT)) {
			pool_instance_lock = std::unique_lock<std::timed_mutex>(poolInfoPtr->poolMutex, std::adopt_lock);
			break;
		}
		if (retries >= ACQUIRE_MAX_RETRIES) {
			LOG_WARNING("VideoPool", "Lock timeout in acquireVideo. Creating temporary fallback instance. Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			auto fallbackInstance = std::make_unique<GStreamerVideo>(monitor);
			fallbackInstance->setSoftOverlay(softOverlay);
			return fallbackInstance;
		}
		std::this_thread::sleep_for(ACQUIRE_BASE_BACKOFF * (1 << retries));
	}
	// --- pool_instance_lock is now held ---

	std::unique_ptr<GStreamerVideo> vid = nullptr;
	bool createdNew = false;

	size_t currentActiveAtomic = poolInfoPtr->currentActive.load(std::memory_order_relaxed);
	size_t currentPooledCount = poolInfoPtr->instances.size();
	size_t currentTotalPopulation = currentActiveAtomic + currentPooledCount;

	size_t targetPopulationCapacity;
	bool isLatched = poolInfoPtr->initialCountLatched.load(std::memory_order_relaxed);

	if (isLatched) {
		targetPopulationCapacity = poolInfoPtr->requiredInstanceCount.load(std::memory_order_relaxed) + POOL_BUFFER_INSTANCES;
	}
	else {
		size_t observedMax = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
		targetPopulationCapacity = observedMax + POOL_BUFFER_INSTANCES;
		// Ensure target is at least POOL_BUFFER_INSTANCES (or 1 if buffer could be 0)
		// Since you want POOL_BUFFER_INSTANCES >= 1, this will always be at least 1.
		if (targetPopulationCapacity < POOL_BUFFER_INSTANCES && POOL_BUFFER_INSTANCES > 0) { // Should only happen if observedMax is 0
			targetPopulationCapacity = POOL_BUFFER_INSTANCES;
		}
		else if (targetPopulationCapacity == 0) { // Only if POOL_BUFFER_INSTANCES was 0 and observedMax was 0
			targetPopulationCapacity = 1; // Still need at least one for the current request
		}
	}

	// Your desired logic: Prioritize creating new if total population is below target capacity
	if (currentTotalPopulation < targetPopulationCapacity) {
		LOG_DEBUG("VideoPool", "Acquire: Current population (" + std::to_string(currentTotalPopulation) +
			") is less than target capacity (" + std::to_string(targetPopulationCapacity) +
			"). Creating new instance. Monitor: " + std::to_string(monitor) + ", ListID: " + std::to_string(listId));
		poolInfoPtr->currentActive.fetch_add(1, std::memory_order_relaxed);
		createdNew = true;
	}
	else if (!poolInfoPtr->instances.empty()) {
		// Target population met (or exceeded), and pool has idle instances: REUSE
		LOG_DEBUG("VideoPool", "Acquire: Target population met. Reusing from pool. Monitor: " +
			std::to_string(monitor) + ", ListID: " + std::to_string(listId));
		vid = std::move(poolInfoPtr->instances.front());
		poolInfoPtr->instances.pop_front();
		poolInfoPtr->currentActive.fetch_add(1, std::memory_order_relaxed);
	}
	else {
		// Target population met (or exceeded), but pool is empty. Wait.
		LOG_DEBUG("VideoPool", "Acquire: Target population met, pool empty. Waiting. Monitor: " +
			std::to_string(monitor) + ", ListID: " + std::to_string(listId) +
			", TargetPop: " + std::to_string(targetPopulationCapacity) +
			", CurrentActive: " + std::to_string(currentActiveAtomic));

		// pool_instance_lock is ALREADY HELD here and is passed to wait_for.
		if (poolInfoPtr->waitCondition.wait_for(pool_instance_lock, ACQUIRE_WAIT_TIMEOUT,
			[poolInfoPtr]() { return !poolInfoPtr->instances.empty(); }))
		{
			// Lock is reacquired by wait_for upon successful wake-up or timeout
			vid = std::move(poolInfoPtr->instances.front());
			poolInfoPtr->instances.pop_front();
			poolInfoPtr->currentActive.fetch_add(1, std::memory_order_relaxed);
			LOG_DEBUG("VideoPool", "Reusing instance from pool after wait. Monitor: " +
				std::to_string(monitor) + ", ListID: " + std::to_string(listId));
		}
		else {
			// Timed out waiting.
			LOG_WARNING("VideoPool", "Timed out waiting for video instance. Creating temporary fallback. Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			// IMPORTANT: pool_instance_lock is still held here after wait_for times out.
			// We must unlock it before returning the fallback, especially since creating GStreamerVideo
			// should ideally not happen under this lock.
			pool_instance_lock.unlock();
			auto fallbackInstance = std::make_unique<GStreamerVideo>(monitor);
			fallbackInstance->setSoftOverlay(softOverlay);
			return fallbackInstance;
		}
	}

	// Update observedMaxActive *before* latching occurs.
	// This uses the current state of the atomic poolInfoPtr->currentActive.
	if (!poolInfoPtr->initialCountLatched.load(std::memory_order_relaxed)) {
		size_t currentActiveAfterThisOperation = poolInfoPtr->currentActive.load(std::memory_order_relaxed);
		size_t currentMax = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
		while (currentActiveAfterThisOperation > currentMax) {
			if (poolInfoPtr->observedMaxActive.compare_exchange_weak(currentMax, currentActiveAfterThisOperation, std::memory_order_relaxed)) break;
			currentMax = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
		}
	}

	// Unlock the pool's mutex BEFORE creating a new GStreamerVideo instance (if createdNew is true)
	// or setting properties on an existing one.
	pool_instance_lock.unlock();

	if (createdNew) {
		vid = std::make_unique<GStreamerVideo>(monitor);
		if (vid && vid->hasError()) {
			LOG_ERROR("VideoPool", "Newly created GStreamerVideo instance has an error. Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
				". It will be discarded upon release by VideoComponent.");
			// To correctly decrement currentActive if construction fails and doesn't throw:
			// This requires re-locking, which adds complexity.
			// A GStreamerVideo constructor that throws on critical failure is cleaner.
			// If it doesn't throw, the 'hasError' will be caught by releaseVideo if it's returned.
			// The currentActive count would be briefly inflated for this faulty instance.
			// For now, let's assume releaseVideo will handle the faulty discard.
			// One option to fix count here if constructor doesn't throw:
			/*
			{
				// Re-lock to adjust count. This is not ideal to lock again.
				std::unique_lock<std::timed_mutex> fixCountLock;
				for (int r = 0; ; ++r) { // Simplified retry for this specific fix
					if (poolInfoPtr->poolMutex.try_lock_for(ACQUIRE_LOCK_TIMEOUT)) {
						fixCountLock = std::unique_lock<std::timed_mutex>(poolInfoPtr->poolMutex, std::adopt_lock);
						poolInfoPtr->currentActive.fetch_sub(1, std::memory_order_relaxed);
						LOG_DEBUG("VideoPool", "Decremented active count for faulty new instance.");
						break;
					}
					if (r >= ACQUIRE_MAX_RETRIES / 2) { // Fewer retries for this internal fix
						LOG_WARNING("VideoPool", "Could not re-lock to decrement active count for faulty new instance.");
						break;
					}
					std::this_thread::sleep_for(ACQUIRE_BASE_BACKOFF);
				}
			}
			// Then you might return nullptr or a fallback.
			// For simplicity, let's assume hasError will be handled by releaseVideo.
			*/
		}
	}

	if (vid) {
		vid->setSoftOverlay(softOverlay);
	}
	else if (!createdNew) {
		// This means vid is null, createdNew is false.
		// This implies it tried to reuse from pool after wait, but move failed (highly unlikely for unique_ptr)
		// OR the fallback path was taken but didn't return early (bug in my example).
		// The fallback path *does* return early, so this branch should ideally not be hit.
		LOG_ERROR("VideoPool", "Internal error: vid is null but was not newly created, and fallback not taken. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		return nullptr;
	}
	else { // createdNew was true, but vid is still null (e.g. make_unique failed with bad_alloc, or GStreamerVideo constructor threw)
		LOG_ERROR("VideoPool", "Failed to construct new GStreamerVideo instance (nullptr after make_unique or exception). Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		// We already incremented currentActive for this. It needs to be decremented.
		// This is a tricky state to recover from perfectly without re-locking.
		// The VideoPool::cleanup due to health check might eventually fix this.
		// For now, just return nullptr.
		return nullptr;
	}

	return std::unique_ptr<IVideo>(std::move(vid));
}
void VideoPool::releaseVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId) {
	// Check if it's a non-pooled instance (listId == -1) or null
	if (!vid || listId == -1) {
		return;
	}

	PoolInfoPtr poolInfoPtr = nullptr;
	bool poolExisted = false;

	// Check if pool exists *without* creating it
	{
		std::shared_lock readLock(mapMutex_);
		auto monitorIt = pools_.find(monitor);
		if (monitorIt != pools_.end()) {
			auto listIt = monitorIt->second.find(listId);
			if (listIt != monitorIt->second.end()) {
				poolInfoPtr = listIt->second;
				poolExisted = true;
			}
		}
	}

	if (!poolExisted) {
		LOG_DEBUG("VideoPool", "Discarding orphaned video instance for List ID: " + std::to_string(listId) +
			" (Monitor: " + std::to_string(monitor) + ") as its pool no longer exists.");
		return; // Let unique_ptr destroy vid
	}

	// --- Pool Existed, proceed with release ---
	bool isFaulty = false;
	if (vid->hasError()) {
		LOG_WARNING("VideoPool", "Faulty video instance detected during release. Discarding. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		isFaulty = true;
	}
	else {
		try {
			vid->unload(); // Prepare for reuse
		}
		catch (const std::exception& e) {
			LOG_ERROR("VideoPool", "Exception during video unload: " + std::string(e.what()) +
				". Discarding instance. Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			isFaulty = true;
		}
		catch (...) {
			LOG_ERROR("VideoPool", "Unknown exception during video unload. Discarding instance. Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			isFaulty = true;
		}
	}

	std::unique_lock<std::timed_mutex> poolLock;
	if (!poolInfoPtr->poolMutex.try_lock_for(RELEASE_LOCK_TIMEOUT)) {
		LOG_WARNING("VideoPool", "Lock timeout in releaseVideo. Discarding instance (and not updating pool counts). Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		// If we can't lock, we can't safely update currentActive or add to instances.
		// The video instance 'vid' will be destroyed when it goes out of scope.
		// This might lead to a slight temporary mismatch in currentActive if it was counted as active.
		return;
	}
	poolLock = std::unique_lock<std::timed_mutex>(poolInfoPtr->poolMutex, std::adopt_lock);

	// Decrement active count. This happens whether the instance is faulty or returned to pool.
	poolInfoPtr->currentActive.fetch_sub(1, std::memory_order_relaxed);
	// size_t activeCountAfterDecrement = poolInfoPtr->currentActive.load(std::memory_order_relaxed); // For logging

	if (isFaulty) {
		LOG_DEBUG("VideoPool", "Discarded faulty instance. Active count updated. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
			", ActiveNow: " + std::to_string(poolInfoPtr->currentActive.load()) +
			", Pooled: " + std::to_string(poolInfoPtr->instances.size()));
		// Faulty instance is not added back to pool. Destruction happens when `vid` unique_ptr goes out of scope.
	}
	else {
		// Healthy instance: return to pool.
		poolInfoPtr->instances.push_back(std::move(vid)); // vid is now nullptr
		LOG_DEBUG("VideoPool", "Instance returned to pool. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
			", ActiveNow: " + std::to_string(poolInfoPtr->currentActive.load()) +
			", Pooled: " + std::to_string(poolInfoPtr->instances.size()));

		// --- Latching Logic (No proactive creation here anymore) ---
		bool wasAlreadyLatched = poolInfoPtr->initialCountLatched.load(std::memory_order_acquire);
		if (!wasAlreadyLatched) {
			size_t peakCount = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
			size_t requiredCount = std::max(peakCount, size_t(1)); // N must be at least 1

			poolInfoPtr->requiredInstanceCount.store(requiredCount, std::memory_order_relaxed);
			poolInfoPtr->initialCountLatched.store(true, std::memory_order_release);

			LOG_INFO("VideoPool", "Initial instance count latched for Monitor: " + std::to_string(monitor) +
				", List ID: " + std::to_string(listId) + ". Observed Peak (N): " + std::to_string(requiredCount) +
				". Target total with buffer will be N + " + std::to_string(POOL_BUFFER_INSTANCES));
			// NO INSTANCE CREATION HERE. acquireVideo will handle reaching N + POOL_BUFFER_INSTANCES.
		}
		// --- End Latching Logic ---

		poolInfoPtr->waitCondition.notify_one(); // Notify one waiting thread an instance is available
	}

	poolLock.unlock();
}

void VideoPool::cleanup(int monitor, int listId) {
	if (listId == -1) return;

	PoolInfoPtr localPoolInfoPtr = nullptr; // To hold the ptr outside map lock

	std::unique_lock mapLock(mapMutex_); // Lock the map first

	LOG_DEBUG("VideoPool", "Starting cleanup for Monitor: " + std::to_string(monitor) +
		", List ID: " + std::to_string(listId));

	auto monitorIt = pools_.find(monitor);
	if (monitorIt != pools_.end()) {
		auto listIt = monitorIt->second.find(listId);
		if (listIt != monitorIt->second.end()) {
			// Get a copy of the shared_ptr
			localPoolInfoPtr = listIt->second;

			// Remove from maps (while mapLock is still held)
			monitorIt->second.erase(listIt);
			if (monitorIt->second.empty()) {
				pools_.erase(monitorIt);
			}
			LOG_DEBUG("VideoPool", "Removed pool entry from map for Monitor: " + std::to_string(monitor) +
				", List ID: " + std::to_string(listId));

		} // else: listId not found, ignore
	} // else: monitor not found, ignore

	mapLock.unlock(); // --- Unlock mapMutex BEFORE locking pool mutex ---

	// Now operate on the local shared_ptr if it was found
	if (localPoolInfoPtr) {
		// Lock the specific pool mutex using the local shared_ptr
		std::unique_lock<std::timed_mutex> poolLock;
		if (!localPoolInfoPtr->poolMutex.try_lock_for(RELEASE_LOCK_TIMEOUT)) { // Use a reasonable timeout
			LOG_WARNING("VideoPool", "Could not lock pool during cleanup *after* map removal for Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
				". Instances will be destroyed when last shared_ptr releases.");
			// No need to do anything else, PoolInfo object will be destroyed
			// when localPoolInfoPtr and any other shared_ptrs go out of scope.
			return;
		}
		poolLock = std::unique_lock<std::timed_mutex>(localPoolInfoPtr->poolMutex, std::adopt_lock);
		// --- Pool Lock Acquired ---

		LOG_DEBUG("VideoPool", "Pool state before cleanup - Active: " +
			std::to_string(localPoolInfoPtr->currentActive.load()) +
			", Pooled: " + std::to_string(localPoolInfoPtr->instances.size()));

		// Clear instances safely under lock
		localPoolInfoPtr->instances.clear(); // unique_ptrs handle destruction

		// Reset pool state safely under lock (optional but good practice)
		localPoolInfoPtr->currentActive.store(0, std::memory_order_relaxed);
		localPoolInfoPtr->observedMaxActive.store(0, std::memory_order_relaxed);
		localPoolInfoPtr->initialCountLatched.store(false, std::memory_order_relaxed);
		localPoolInfoPtr->requiredInstanceCount.store(0, std::memory_order_relaxed);

		// Pool lock releases automatically when poolLock goes out of scope

		LOG_DEBUG("VideoPool", "Completed instance clearing for Monitor: " + std::to_string(monitor) +
			", List ID: " + std::to_string(listId));
	}
	// localPoolInfoPtr goes out of scope here. If this was the last shared_ptr,
	// the PoolInfo object is destroyed.
}

void VideoPool::shutdown() {
	LOG_INFO("VideoPool", "Starting VideoPool shutdown...");

	// Vector to hold shared_ptrs to ensure PoolInfo objects survive map clearing
	std::vector<PoolInfoPtr> poolInfosToClean;
	std::vector<std::pair<int, int>> poolKeys; // Keep track of keys for logging

	// Phase 1: Collect pointers and clear the map under map lock
	{
		std::unique_lock mapLock(mapMutex_);
		LOG_DEBUG("VideoPool", "Collecting pool pointers and clearing map...");

		for (auto const& [monitor, listMap] : pools_) {
			for (auto const& [listId, poolInfoPtr] : listMap) {
				poolInfosToClean.push_back(poolInfoPtr); // Copy shared_ptr
				poolKeys.emplace_back(monitor, listId); // Store keys for logging later
			}
		}
		pools_.clear(); // Clear the main map entirely

		LOG_DEBUG("VideoPool", "Pool map cleared.");
		// mapLock is released here
	}

	// Phase 2: Clean up each pool individually without holding the map lock
	LOG_DEBUG("VideoPool", "Cleaning up individual pools...");
	for (size_t i = 0; i < poolInfosToClean.size(); ++i) {
		PoolInfoPtr& poolInfoPtr = poolInfosToClean[i];
		int monitor = poolKeys[i].first;
		int listId = poolKeys[i].second;

		LOG_DEBUG("VideoPool", "Attempting shutdown cleanup for Monitor: " + std::to_string(monitor) +
			", List ID: " + std::to_string(listId));

		// Try to lock the individual pool
		// Use a short timeout or simple try_lock as it's shutdown anyway
		if (poolInfoPtr->poolMutex.try_lock()) { // Or try_lock_for with a small timeout
			std::lock_guard poolLock(poolInfoPtr->poolMutex, std::adopt_lock);

			LOG_DEBUG("VideoPool", "Clearing instances for Monitor: " + std::to_string(monitor) +
				", List ID: " + std::to_string(listId));

			poolInfoPtr->instances.clear(); // Clear instances (unique_ptrs handle destruction)
			// Resetting state is technically optional as the PoolInfo will be destroyed,
			// but good practice if PoolInfo could somehow survive.
			poolInfoPtr->currentActive.store(0, std::memory_order_relaxed);
			poolInfoPtr->observedMaxActive.store(0, std::memory_order_relaxed);
			poolInfoPtr->initialCountLatched.store(false, std::memory_order_relaxed);
			poolInfoPtr->requiredInstanceCount.store(0, std::memory_order_relaxed);
			// poolLock releases automatically
		}
		else {
			LOG_WARNING("VideoPool", "Could not lock pool during shutdown cleanup: Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
				". Instances will be destroyed when last shared_ptr reference drops.");
			// We don't wait. The PoolInfo and its contents will be destroyed when
			// poolInfoPtr (and any other references) go out of scope.
		}
	} // End loop through collected pointers

	LOG_INFO("VideoPool", "VideoPool shutdown complete.");
	// poolInfosToClean vector is destroyed here. If any PoolInfo object's
	// reference count hits zero, it and its contained deque/unique_ptrs
	// will be destroyed.
}

bool VideoPool::checkPoolHealth(int monitor, int listId) {
	std::shared_lock readLock(mapMutex_); // Shared lock is sufficient for reading

	auto monitorIt = pools_.find(monitor);
	if (monitorIt != pools_.end()) {
		auto listIt = monitorIt->second.find(listId);
		if (listIt != monitorIt->second.end()) {
			// Get the shared_ptr safely under the map lock
			PoolInfoPtr poolInfoPtr = listIt->second;

			// Read the atomic count directly. No need to lock poolInfoPtr->poolMutex
			// This is safe because the PoolInfo object's lifetime is guaranteed
			// by the shared_ptr while we hold the map lock and the local copy.
			size_t activeCount = poolInfoPtr->currentActive.load(std::memory_order_relaxed);

			if (activeCount > HEALTH_CHECK_ACTIVE_THRESHOLD) { // Use the constant
				LOG_WARNING("VideoPool", "Health check detected suspicious active count: " +
					std::to_string(activeCount) + " for Monitor: " + std::to_string(monitor) +
					", List ID: " + std::to_string(listId) + ". Scheduling cleanup might be needed.");
				// Note: This function now only *detects*. It doesn't schedule cleanup itself.
				// The caller (e.g., acquireVideo) decides based on the return value.
				return false; // Indicates potential problem
			}
			return true; // Pool exists and count is okay
		}
	}
	// Pool doesn't exist (or monitor doesn't)
	return true; // Healthy by default if it doesn't exist
}