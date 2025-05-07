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

VideoPool::PoolInfo* VideoPool::getPoolInfo(int monitor, int listId) {
    // Try read-only access first
    {
        std::shared_lock readLock(mapMutex_);
        auto monitorIt = pools_.find(monitor);
        if (monitorIt != pools_.end()) {
            auto listIt = monitorIt->second.find(listId);
            if (listIt != monitorIt->second.end()) {
                return &listIt->second;
            }
        }
    } // readLock released here

    // Need to create new pool info, acquire write lock
    std::unique_lock writeLock(mapMutex_);
    // Check again after acquiring write lock (double-checked locking pattern)
    auto monitorIt = pools_.find(monitor);
    if (monitorIt != pools_.end()) {
        auto listIt = monitorIt->second.find(listId);
        if (listIt != monitorIt->second.end()) {
            return &listIt->second; // Another thread created it
        }
        // Monitor exists, but listId doesn't
        LOG_DEBUG("VideoPool", "Creating new pool entry for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return &monitorIt->second[listId]; // Create listId entry
    }
    // Neither monitor nor listId exists
    LOG_DEBUG("VideoPool", "Creating new pool entry for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    return &pools_[monitor][listId]; // Create monitor and listId entries
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

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Acquire lock for the specific pool with backoff
    std::unique_lock<std::timed_mutex> poolLock;
    for (int retries = 0; ; ++retries) {
        if (poolInfo->poolMutex.try_lock_for(ACQUIRE_LOCK_TIMEOUT)) {
            poolLock = std::unique_lock<std::timed_mutex>(poolInfo->poolMutex, std::adopt_lock);
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
    if (!poolInfo->instances.empty()) {
        vid = std::move(poolInfo->instances.front());
        poolInfo->instances.pop_front();
        poolInfo->currentActive.fetch_add(1, std::memory_order_relaxed);
        LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    }
    else {
        // 2. Pool empty. Determine if we need to create or wait.
        size_t currentActive = poolInfo->currentActive.load(std::memory_order_relaxed);
        size_t currentPooled = poolInfo->instances.size(); // Will be 0 here
        size_t currentTotal = currentActive + currentPooled;

        size_t targetTotalInstances;
        bool isLatched = poolInfo->initialCountLatched.load(std::memory_order_relaxed);

        if (isLatched) {
            // Target is fixed after latching
            targetTotalInstances = poolInfo->requiredInstanceCount.load(std::memory_order_relaxed) + 1;
        }
        else {
            // Before latching, target grows with observed max (minimum 1 means target is at least 2)
            // We only create *new* ones before latching, up to the observed peak + 1
            size_t observedMax = poolInfo->observedMaxActive.load(std::memory_order_relaxed);
            targetTotalInstances = std::max(observedMax, currentTotal) + 1; // Ensure target includes current request
            // Use currentTotal in max ensures we try to create if observedMax is stale/low
        }

        if (currentTotal < targetTotalInstances) {
            // Need to create a new instance (either pre-latch growth or post-latch replenishment)
            poolInfo->currentActive.fetch_add(1, std::memory_order_relaxed); // Increment before creating
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

            if (poolInfo->waitCondition.wait_for(poolLock, ACQUIRE_WAIT_TIMEOUT,
                [poolInfo]() { return !poolInfo->instances.empty(); }))
            {
                // Got an instance after waiting
                vid = std::move(poolInfo->instances.front());
                poolInfo->instances.pop_front();
                poolInfo->currentActive.fetch_add(1, std::memory_order_relaxed);
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
    if (!poolInfo->initialCountLatched.load(std::memory_order_relaxed)) {
        size_t currentActiveAfterUpdate = poolInfo->currentActive.load(std::memory_order_relaxed);
        size_t currentMax = poolInfo->observedMaxActive.load(std::memory_order_relaxed);
        // Update observedMaxActive if the new active count is higher
        while (currentActiveAfterUpdate > currentMax) {
            if (poolInfo->observedMaxActive.compare_exchange_weak(currentMax, currentActiveAfterUpdate, std::memory_order_relaxed)) break;
            // Reload currentMax if CAS failed due to contention
            currentMax = poolInfo->observedMaxActive.load(std::memory_order_relaxed);
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

void VideoPool::releaseVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId) {
    // Check if it's a non-pooled instance (listId == -1) or null
    // Note: Fallback instances created due to timeouts also won't have pool info
    // associated implicitly, they just get destroyed by unique_ptr.
    // We only handle instances that were originally acquired *with* a valid listId.
    if (!vid || listId == -1) {
        // Let unique_ptr handle destruction of non-pooled or fallback instances
        return;
    }

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);
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

    // Lock the pool
    std::unique_lock<std::timed_mutex> poolLock;
    if (!poolInfo->poolMutex.try_lock_for(RELEASE_LOCK_TIMEOUT)) {
        LOG_WARNING("VideoPool", "Lock timeout in releaseVideo. Discarding instance. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return; // Let unique_ptr destroy vid
    }
    poolLock = std::unique_lock<std::timed_mutex>(poolInfo->poolMutex, std::adopt_lock);

    // --- Lock Acquired ---

    // Decrement active count regardless of fault status
    poolInfo->currentActive.fetch_sub(1, std::memory_order_relaxed);
    size_t activeCountAfterDecrement = poolInfo->currentActive.load(std::memory_order_relaxed); // Read decremented value

    if (isFaulty) {
        // Faulty instance: just discard. Replacement happens on demand in acquireVideo.
        LOG_DEBUG("VideoPool", "Discarded faulty instance. Active count decremented. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        // Let unique_ptr destroy vid when it goes out of scope after unlock
    }
    else {
        // Healthy instance: return to pool and potentially latch/create buffer.

        bool countWasLatched = poolInfo->initialCountLatched.load(std::memory_order_acquire);

        // Latch the required count on the *first* successful release
        if (!countWasLatched) {
            size_t peakCount = poolInfo->observedMaxActive.load(std::memory_order_relaxed);
            size_t requiredCount = std::max(peakCount, size_t(1));
            poolInfo->requiredInstanceCount.store(requiredCount, std::memory_order_relaxed);
            poolInfo->initialCountLatched.store(true, std::memory_order_release);
            countWasLatched = true; // Mark as latched for logic below
            LOG_INFO("VideoPool", "Initial instance count latched for Monitor: " + std::to_string(monitor) +
                ", List ID: " + std::to_string(listId) + ". Required count: " + std::to_string(requiredCount) +
                " (Pool target total: " + std::to_string(requiredCount + 1) + ")");
        }

        // Add the healthy, unloaded instance back to the pool (becomes most recently idle)
        poolInfo->instances.push_back(std::move(vid));
        size_t pooledCountAfterAdd = poolInfo->instances.size(); // Includes the one just added

        LOG_DEBUG("VideoPool", "Instance returned to pool. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));


        // --- Proactive Buffer Creation Logic ---
        if (countWasLatched) {
            size_t currentTotal = activeCountAfterDecrement + pooledCountAfterAdd;
            size_t requiredCount = poolInfo->requiredInstanceCount.load(std::memory_order_relaxed);
            size_t targetTotal = requiredCount + 1;

            // If current total is less than the target (N+1), create the missing buffer instance(s) proactively.
            // This usually happens right after latching when total = N.
            if (currentTotal < targetTotal) {
                // In theory, could be < targetTotal by more than 1 if multiple instances
                // were faulty and discarded previously, but we top up to targetTotal.
                size_t needed = targetTotal - currentTotal;
                LOG_INFO("VideoPool", "Proactively creating " + std::to_string(needed) +
                    " buffer instance(s) to reach target " + std::to_string(targetTotal) +
                    ". Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
                for (size_t i = 0; i < needed; ++i) {
                    // Add new idle instances to the back (they become most-recently-idle)
                    poolInfo->instances.push_back(std::make_unique<GStreamerVideo>(monitor));
                }
            }
        }
        // --- End Proactive Buffer Creation ---

         // Notify one waiting thread (if any) that an instance is available
         // Do this AFTER potential buffer creation and BEFORE unlocking is safest practice.
         // poolLock still held here.
        poolInfo->waitCondition.notify_one(); // Notify *under lock* recommended for CVs

    } // End of healthy instance handling

    poolLock.unlock(); // Explicitly unlock before returning

    // If vid was faulty, it gets destroyed here by unique_ptr going out of scope.
    // If it was healthy, it was moved into the pool's deque.
}


void VideoPool::cleanup(int monitor, int listId) {
    if (listId == -1) return;

    std::unique_lock mapLock(mapMutex_); // Lock the map first

    LOG_DEBUG("VideoPool", "Starting cleanup for Monitor: " + std::to_string(monitor) +
        ", List ID: " + std::to_string(listId));

    auto monitorIt = pools_.find(monitor);
    if (monitorIt != pools_.end()) {
        auto listIt = monitorIt->second.find(listId);
        if (listIt != monitorIt->second.end()) {
            PoolInfo& poolInfo = listIt->second;

            // Lock the specific pool before modifying
            std::unique_lock<std::timed_mutex> poolLock;
            if (!poolInfo.poolMutex.try_lock_for(RELEASE_LOCK_TIMEOUT)) { // Use a reasonable timeout
                LOG_WARNING("VideoPool", "Could not lock pool during cleanup for Monitor: " +
                    std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                    ". Skipping detailed cleanup, removing from map.");
                // Remove from maps even if we can't lock it
                monitorIt->second.erase(listIt);
                if (monitorIt->second.empty()) {
                    pools_.erase(monitorIt);
                }
                return; // Exit, mapLock releases
            }
            poolLock = std::unique_lock<std::timed_mutex>(poolInfo.poolMutex, std::adopt_lock);
            // --- Pool Lock Acquired ---

            LOG_DEBUG("VideoPool", "Pool state before cleanup - Active: " +
                std::to_string(poolInfo.currentActive.load()) +
                ", Pooled: " + std::to_string(poolInfo.instances.size()));

            poolInfo.instances.clear(); // unique_ptrs handle destruction

            // Reset pool state safely under lock
            poolInfo.currentActive.store(0, std::memory_order_relaxed);
            poolInfo.observedMaxActive.store(0, std::memory_order_relaxed); // Reset pre-latch counter too
            poolInfo.initialCountLatched.store(false, std::memory_order_relaxed); // Reset latch state
            poolInfo.requiredInstanceCount.store(0, std::memory_order_relaxed); // Reset required count

            // Pool lock releases automatically

            // Remove from maps (while mapLock is still held)
            monitorIt->second.erase(listIt);
            if (monitorIt->second.empty()) {
                pools_.erase(monitorIt);
            }

            LOG_DEBUG("VideoPool", "Completed cleanup for Monitor: " + std::to_string(monitor) +
                ", List ID: " + std::to_string(listId));
        } // else: listId not found, ignore
    } // else: monitor not found, ignore
    // mapLock releases here
}

void VideoPool::shutdown() {
    std::unique_lock mapLock(mapMutex_); // Lock the main map

    LOG_INFO("VideoPool", "Starting VideoPool shutdown..."); // Use INFO level for shutdown

    for (auto itMon = pools_.begin(); itMon != pools_.end(); /* no increment here */) {
        int monitor = itMon->first;
        auto& listPools = itMon->second;
        for (auto itList = listPools.begin(); itList != listPools.end(); /* no increment here */) {
            int listId = itList->first;
            PoolInfo& poolInfo = itList->second;

            LOG_DEBUG("VideoPool", "Shutting down pool for Monitor: " + std::to_string(monitor) +
                ", List ID: " + std::to_string(listId));

            // Try to lock the individual pool
            if (!poolInfo.poolMutex.try_lock()) {
                LOG_WARNING("VideoPool", "Skipping busy pool during shutdown: Monitor: " +
                    std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                    ". Instances may not be cleaned up immediately.");
                ++itList; // Move to next listId for this monitor
                continue; // Skip this pool
            }
            // --- Pool Lock Acquired ---
            std::lock_guard poolLock(poolInfo.poolMutex, std::adopt_lock);
            poolInfo.instances.clear(); // Clear instances (unique_ptrs handle destruction)
            // Resetting state is optional here as the entry will be removed, but doesn't hurt
            poolInfo.currentActive.store(0);
            poolInfo.observedMaxActive.store(0);
            poolInfo.initialCountLatched.store(false);
            poolInfo.requiredInstanceCount.store(0);

            // Pool lock releases automatically

            // Erase the current listId entry and advance the iterator
            itList = listPools.erase(itList);
        } // End inner loop (listIds)

        // If the inner map is now empty after erasing, erase the monitor entry
        if (listPools.empty()) {
            LOG_DEBUG("VideoPool", "Removing empty monitor entry during shutdown: " + std::to_string(monitor));
            itMon = pools_.erase(itMon);
        }
        else {
            ++itMon; // Otherwise, just move to the next monitor
        }
    } // End outer loop (monitors)

    // pools_.clear(); // No longer needed, erase handles removal

    LOG_INFO("VideoPool", "VideoPool shutdown complete"); // Use INFO level
    // mapLock releases here
}

bool VideoPool::checkPoolHealth(int monitor, int listId) {
    std::shared_lock readLock(mapMutex_);
    if (pools_.count(monitor) && pools_[monitor].count(listId)) {
        PoolInfo const& poolInfo = pools_[monitor][listId];

        // If active count is suspiciously high, reset pool
        size_t activeCount = poolInfo.currentActive.load();

        if (activeCount > 20) {  // Very high count - likely corruption
            LOG_WARNING("VideoPool", "Health check detected suspicious active count: " +
                std::to_string(activeCount) + " for Monitor: " + std::to_string(monitor) +
                ", List ID: " + std::to_string(listId) + ". Scheduling cleanup.");
            return false;
        }

        return true;
    }
    return true;  // No pool exists, so it's "healthy" by default
}
