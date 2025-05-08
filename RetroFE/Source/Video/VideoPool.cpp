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
		instance->setSoftOverlay(softOverlay); // Set properties if needed directly
		return instance; // Return as IVideo
	}

	// Periodic health check
	static std::atomic<int> callCounter{ 0 };
	if (callCounter.fetch_add(1, std::memory_order_relaxed) % HEALTH_CHECK_INTERVAL == (HEALTH_CHECK_INTERVAL - 1)) {
		if (!checkPoolHealth(monitor, listId)) {
			// Health check failed, cleanup resets the pool.
			cleanup(monitor, listId);
			// Fall through to potentially create a new instance in a fresh pool
		}
	}

	PoolInfoPtr poolInfoPtr = getPoolInfo(monitor, listId);

	// Acquire lock for the specific pool with backoff
	std::unique_lock<std::timed_mutex> poolLock;
	for (int retries = 0; ; ++retries) {
		if (poolInfoPtr->poolMutex.try_lock_for(ACQUIRE_LOCK_TIMEOUT)) {
			poolLock = std::unique_lock<std::timed_mutex>(poolInfoPtr->poolMutex, std::adopt_lock);
			break;
		}
		if (retries >= ACQUIRE_MAX_RETRIES) {
			LOG_WARNING("VideoPool", "Lock timeout in acquireVideo. Creating temporary fallback instance. Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			// Create a temporary instance, don't modify pool state otherwise.
			// It will be destroyed when the caller's unique_ptr goes out of scope.
			auto fallbackInstance = std::make_unique<GStreamerVideo>(monitor);
			fallbackInstance->setSoftOverlay(softOverlay);
			return fallbackInstance; // Return as IVideo
		}
		std::this_thread::sleep_for(ACQUIRE_BASE_BACKOFF * (1 << retries));
	}

	// --- Lock Acquired ---

	std::unique_ptr<GStreamerVideo> vid = nullptr;
	bool createdNew = false;

	// 1. Check if available in the pool
	if (!poolInfoPtr->instances.empty()) {
		vid = std::move(poolInfoPtr->instances.front());
		poolInfoPtr->instances.pop_front();
		poolInfoPtr->currentActive.fetch_add(1, std::memory_order_relaxed);
		LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
	}
	else {
		// 2. Pool empty. Determine if we need to create or wait.
		size_t currentActive = poolInfoPtr->currentActive.load(std::memory_order_relaxed);
		size_t currentPooled = poolInfoPtr->instances.size(); // Will be 0 here
		size_t currentTotal = currentActive + currentPooled;

		size_t targetTotalInstances;
		bool isLatched = poolInfoPtr->initialCountLatched.load(std::memory_order_relaxed);

		if (isLatched) {
			// Target is fixed after latching
			targetTotalInstances = poolInfoPtr->requiredInstanceCount.load(std::memory_order_relaxed) + 1;
		}
		else {
			// Before latching, target grows with observed max (minimum 1 means target is at least 2)
			// We only create *new* ones before latching, up to the observed peak + 1
			size_t observedMax = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
			targetTotalInstances = std::max(observedMax, currentTotal) + 1; // Ensure target includes current request
			// Use currentTotal in max ensures we try to create if observedMax is stale/low
		}

		if (currentTotal < targetTotalInstances) {
			// Need to create a new instance (either pre-latch growth or post-latch replenishment)
			poolInfoPtr->currentActive.fetch_add(1, std::memory_order_relaxed); // Increment before creating
			createdNew = true;
			LOG_DEBUG("VideoPool", "Creating new instance (pool empty, below target). Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
				", ActiveCountAfter: " + std::to_string(currentActive + 1) +
				", TargetTotal: " + std::to_string(targetTotalInstances) +
				", Latched: " + (isLatched ? "Yes" : "No"));
			// Create the instance *after* unlocking
		}
		else {
			// Pool is empty, but we have reached the target total. Wait for one to be released.
			LOG_DEBUG("VideoPool", "Pool empty, target reached. Waiting for instance. Monitor: " +
				std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
				", TargetTotal: " + std::to_string(targetTotalInstances));

			if (poolInfoPtr->waitCondition.wait_for(poolLock, ACQUIRE_WAIT_TIMEOUT,
				[poolInfoPtr]() { return !poolInfoPtr->instances.empty(); }))
			{
				// Got an instance after waiting
				vid = std::move(poolInfoPtr->instances.front());
				poolInfoPtr->instances.pop_front();
				poolInfoPtr->currentActive.fetch_add(1, std::memory_order_relaxed);
				LOG_DEBUG("VideoPool", "Reusing instance from pool after wait. Monitor: " +
					std::to_string(monitor) + ", List ID: " + std::to_string(listId));
			}
			else {
				// Timed out waiting. Create a temporary fallback.
				LOG_WARNING("VideoPool", "Timed out waiting for video instance. Creating temporary fallback. Monitor: " +
					std::to_string(monitor) + ", List ID: " + std::to_string(listId));
				// Unlock before creating fallback
				poolLock.unlock();
				auto fallbackInstance = std::make_unique<GStreamerVideo>(monitor);
				fallbackInstance->setSoftOverlay(softOverlay);
				return fallbackInstance; // Return as IVideo
			}
		}
	}

	// Update observedMaxActive *before* latching occurs
	if (!poolInfoPtr->initialCountLatched.load(std::memory_order_relaxed)) {
		size_t currentActiveAfterUpdate = poolInfoPtr->currentActive.load(std::memory_order_relaxed);
		size_t currentMax = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
		// Update observedMaxActive if the new active count is higher
		while (currentActiveAfterUpdate > currentMax) {
			if (poolInfoPtr->observedMaxActive.compare_exchange_weak(currentMax, currentActiveAfterUpdate, std::memory_order_relaxed)) break;
			// Reload currentMax if CAS failed due to contention
			currentMax = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
		}
	}

	// Unlock mutex before creating the video instance or setting properties
	poolLock.unlock();

	if (createdNew) {
		vid = std::make_unique<GStreamerVideo>(monitor);
	}

	// Set properties on the acquired/created instance
	if (vid) {
		vid->setSoftOverlay(softOverlay);
	}
	else if (!createdNew) {
		// This case should ideally not happen if logic is correct (either reused, created new, or returned fallback)
		LOG_ERROR("VideoPool", "Internal error: Failed to acquire or create video instance unexpectedly.");
		return nullptr;
	}


	return std::unique_ptr<IVideo>(std::move(vid));
}

#include <shared_mutex> // Make sure this is included for std::shared_lock

// ... other includes ...

void VideoPool::releaseVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId) {
	// Check if it's a non-pooled instance (listId == -1) or null
	if (!vid || listId == -1) {
		// Let unique_ptr handle destruction of non-pooled or fallback instances
		return;
	}

	// --- MODIFICATION START: Check if pool exists BEFORE proceeding ---
	PoolInfoPtr poolInfoPtr = nullptr; // Start with null
	bool poolExisted = false;

	{ // Scope for the read lock
		std::shared_lock readLock(mapMutex_);
		auto monitorIt = pools_.find(monitor);
		if (monitorIt != pools_.end()) {
			auto listIt = monitorIt->second.find(listId);
			if (listIt != monitorIt->second.end()) {
				// Pool exists, get a reference to its shared_ptr
				poolInfoPtr = listIt->second;
				poolExisted = true;
			}
		}
	} // readLock is released here

	if (!poolExisted) {
		// The pool this video supposedly belongs to doesn't exist anymore (likely cleaned up).
		// Discard the video without creating a new pool entry just for this return.
		LOG_DEBUG("VideoPool", "Discarding orphaned video instance for List ID: " + std::to_string(listId) +
			" (Monitor: " + std::to_string(monitor) + ") as its pool no longer exists.");
		// Let unique_ptr destroy vid when it goes out of scope
		return;
	}
	// --- MODIFICATION END ---

	// --- Original logic continues below, but now we know poolInfoPtr is valid ---

	bool isFaulty = false;

	// Check for errors before attempting unload
	if (vid->hasError()) {
		LOG_WARNING("VideoPool", "Faulty video instance detected during release. Discarding. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		isFaulty = true;
	}
	else {
		// Try to unload cleanly
		try {
			vid->unload();
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

	// Lock the pool (We know poolInfoPtr is valid because poolExisted was true)
	std::unique_lock<std::timed_mutex> poolLock;
	if (!poolInfoPtr->poolMutex.try_lock_for(RELEASE_LOCK_TIMEOUT)) {
		LOG_WARNING("VideoPool", "Lock timeout in releaseVideo. Discarding instance. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		// Let unique_ptr destroy vid if lock fails
		return;
	}
	poolLock = std::unique_lock<std::timed_mutex>(poolInfoPtr->poolMutex, std::adopt_lock);

	// --- Lock Acquired ---

	// Decrement active count regardless of fault status
	poolInfoPtr->currentActive.fetch_sub(1, std::memory_order_relaxed);
	size_t activeCountAfterDecrement = poolInfoPtr->currentActive.load(std::memory_order_relaxed); // Read decremented value

	if (isFaulty) {
		// Faulty instance: just discard. Replacement happens on demand in acquireVideo.
		LOG_DEBUG("VideoPool", "Discarded faulty instance. Active count decremented. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));
		// Let unique_ptr destroy vid when it goes out of scope after unlock
	}
	else {
		// Healthy instance: return to pool and potentially latch/create buffer.

		bool countWasLatched = poolInfoPtr->initialCountLatched.load(std::memory_order_acquire);

		// Latch the required count on the *first* successful release for this pool instance
		if (!countWasLatched) {
			size_t peakCount = poolInfoPtr->observedMaxActive.load(std::memory_order_relaxed);
			// Ensure requiredCount is at least 1, even if peak was 0 (e.g., only foreign returns happened)
			size_t requiredCount = std::max(peakCount, size_t(1));
			poolInfoPtr->requiredInstanceCount.store(requiredCount, std::memory_order_relaxed);
			poolInfoPtr->initialCountLatched.store(true, std::memory_order_release);
			countWasLatched = true; // Mark as latched for logic below
			LOG_INFO("VideoPool", "Initial instance count latched for Monitor: " + std::to_string(monitor) +
				", List ID: " + std::to_string(listId) + ". Required count: " + std::to_string(requiredCount) +
				" (Pool target total: " + std::to_string(requiredCount + 1) + ")");
		}

		// Add the healthy, unloaded instance back to the pool (becomes most recently idle)
		poolInfoPtr->instances.push_back(std::move(vid)); // vid is now nullptr
		size_t pooledCountAfterAdd = poolInfoPtr->instances.size(); // Includes the one just added

		LOG_DEBUG("VideoPool", "Instance returned to pool. Monitor: " +
			std::to_string(monitor) + ", List ID: " + std::to_string(listId));

		// --- Proactive Buffer Creation Logic ---
		// This logic still runs, but now only for pools that actually exist when releaseVideo is called.
		if (countWasLatched) {
			// Need to be careful with activeCountAfterDecrement if it wrapped around.
			// A simple check: if activeCountAfterDecrement is very large, treat it as 0 for sizing.
			size_t currentActiveForSizing = activeCountAfterDecrement;
			// Define "very large" relative to a reasonable maximum expected active count. SIZE_MAX/2 is safe.
			if (currentActiveForSizing > SIZE_MAX / 2) {
				LOG_DEBUG("VideoPool", "Adjusting wrapped active count for sizing calculation. List ID: " + std::to_string(listId));
				currentActiveForSizing = 0;
			}

			size_t currentTotal = currentActiveForSizing + pooledCountAfterAdd;
			size_t requiredCount = poolInfoPtr->requiredInstanceCount.load(std::memory_order_relaxed);
			size_t targetTotal = requiredCount + 1;

			if (currentTotal < targetTotal) {
				size_t needed = targetTotal - currentTotal;
				LOG_INFO("VideoPool", "Proactively creating " + std::to_string(needed) +
					" buffer instance(s) to reach target " + std::to_string(targetTotal) +
					". Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
				for (size_t i = 0; i < needed; ++i) {
					poolInfoPtr->instances.push_back(std::make_unique<GStreamerVideo>(monitor));
				}
			}
			else if (currentTotal > targetTotal + 1) { // Optional: Trim excess if pool grew too large due to foreign returns?
				// This is more complex - requires deciding which instance to trim (e.g., oldest idle).
				// For simplicity, let's skip trimming for now. The pool might just be slightly larger than N+1 temporarily.
				LOG_DEBUG("VideoPool", "Pool size (" + std::to_string(currentTotal) +
				          ") exceeds target+1 (" + std::to_string(targetTotal+1) + "). No trimming implemented. List ID: " + std::to_string(listId));
			}
		}
		// --- End Proactive Buffer Creation ---

		 // Notify one waiting thread (if any) that an instance is available
		poolInfoPtr->waitCondition.notify_one(); // Notify *under lock*

	} // End of healthy instance handling

	poolLock.unlock(); // Explicitly unlock before returning

	// If vid was faulty, it gets destroyed here by unique_ptr going out of scope.
	// If it was healthy, it was moved into the pool's deque (vid is nullptr now).
	// If the pool didn't exist initially, vid gets destroyed here.
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