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

VideoPool::PoolMap VideoPool::pools_;
std::shared_mutex VideoPool::mapMutex_;

VideoPool::PoolInfo* VideoPool::getPoolInfo(int monitor, int listId) {
    // Try read-only access first
    {
        std::shared_lock readLock(mapMutex_);
        if (pools_.count(monitor) && pools_[monitor].count(listId)) {
            return &pools_[monitor][listId];
        }
    }

    // Need to create new pool info, acquire write lock
    std::unique_lock writeLock(mapMutex_);
    return &pools_[monitor][listId];
}

std::unique_ptr<IVideo> VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        return std::make_unique<GStreamerVideo>(monitor);
    }

    // Periodically check health and trim excess instances (every 30th call)
    static std::atomic<int> callCounter{ 0 };
    if (++callCounter % 30 == 0) {
        if (!checkPoolHealth(monitor, listId)) {
            // If health check fails, schedule cleanup
            cleanup(monitor, listId);
        }
        else {
            // If health is good, just trim excess instances
            trimExcessInstances(monitor, listId);
        }
    }

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Add backoff mechanism for lock acquisition
    const int MAX_RETRIES = 5;
    int retries = 0;
    while (!poolInfo->poolMutex.try_lock_for(std::chrono::milliseconds(100))) {
        if (++retries >= MAX_RETRIES) {
            LOG_WARNING("VideoPool", "Lock timeout in acquireVideo. Creating fallback instance. Monitor: " +
                std::to_string(monitor) + ", List ID: " + std::to_string(listId));
            return std::make_unique<GStreamerVideo>(monitor);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * retries));  // Exponential backoff
    }

    std::unique_lock poolLock(poolInfo->poolMutex, std::adopt_lock);

    // If not initialized yet, create new instances freely
    if (!poolInfo->poolInitialized.load()) {
        poolInfo->currentActive.fetch_add(1);
        LOG_DEBUG("VideoPool", "Creating initial instance. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return std::make_unique<GStreamerVideo>(monitor);
    }

    // If we haven't created our +1 extra instance yet, do it now
    if (!poolInfo->hasExtraInstance.load()) {
        poolInfo->hasExtraInstance.store(true);
        poolInfo->currentActive.fetch_add(1);
        LOG_DEBUG("VideoPool", "Creating +1 extra instance. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return std::make_unique<GStreamerVideo>(monitor);
    }

    auto waitResult = poolInfo->waitCondition.wait_for(poolLock,
        std::chrono::milliseconds(500),
        [poolInfo]() { return !poolInfo->instances.empty(); });

    if (!waitResult) {
        // Timed out waiting for an instance
        LOG_WARNING("VideoPool", "Timed out waiting for video instance. Creating new instance. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        poolInfo->currentActive.fetch_add(1);
        return std::make_unique<GStreamerVideo>(monitor);
    }

    std::unique_ptr<GStreamerVideo> vid = std::move(poolInfo->instances.front());
    poolInfo->instances.pop_front();
    vid->setSoftOverlay(softOverlay);
    poolInfo->currentActive.fetch_add(1);

    // After incrementing currentActive, update observedMaxActive if needed
    size_t newActiveCount = poolInfo->currentActive.load();
    size_t currentMax = poolInfo->observedMaxActive.load();
    if (newActiveCount > currentMax) {
        poolInfo->observedMaxActive.store(newActiveCount);
    }

    LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " +
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    return std::unique_ptr<IVideo>(std::move(vid));
}

void VideoPool::releaseVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId) {
    if (!vid || listId == -1) return;

    // Check if the instance encountered an error.
    if (vid->hasError()) {
        LOG_DEBUG("VideoPool", "Faulty video instance detected during release. Destroying instance. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        destroyVideo(std::move(vid), monitor, listId);
        return;
    }

    try {
        vid->unload();
    }
    catch (const std::exception& e) {
        LOG_ERROR("VideoPool", "Exception during video unload: " + std::string(e.what()) +
            ". Destroying instance.");
        destroyVideo(std::move(vid), monitor, listId);
        return;
    }
    catch (...) {
        LOG_ERROR("VideoPool", "Unknown exception during video unload. Destroying instance.");
        destroyVideo(std::move(vid), monitor, listId);
        return;
    }

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    if (!poolInfo->poolMutex.try_lock_for(std::chrono::milliseconds(300))) {
        LOG_WARNING("VideoPool", "Lock timeout in releaseVideo. Destroying instance. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        destroyVideo(std::move(vid), monitor, listId);
        return;
    }
    std::unique_lock poolLock(poolInfo->poolMutex, std::adopt_lock);

    // Decrement the active count under lock so that the instance can be accounted for.
    poolInfo->currentActive.fetch_sub(1);

    // On the first release, initialize the pool and set initial observed max
    if (!poolInfo->poolInitialized.load()) {
        poolInfo->poolInitialized.store(true);
        poolInfo->instances.push_back(std::move(vid));

        // Initial observed max should be at least 1
        size_t currentActive = poolInfo->currentActive.load();
        poolInfo->observedMaxActive.store(std::max(currentActive, size_t(1)));

        LOG_DEBUG("VideoPool", "First release detected for Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        poolInfo->waitCondition.notify_one();
        return;
    }

    // Return the instance to the pool.
    poolInfo->instances.push_back(std::move(vid));
    poolInfo->waitCondition.notify_one();
    LOG_DEBUG("VideoPool", "Instance added to pool. Monitor: " +
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::cleanup(int monitor, int listId) {
    if (listId == -1) return;

    std::unique_lock mapLock(mapMutex_);

    // Log the cleanup start
    LOG_DEBUG("VideoPool", "Starting cleanup for Monitor: " + std::to_string(monitor) +
        ", List ID: " + std::to_string(listId));

    if (pools_.count(monitor) && pools_[monitor].count(listId)) {
        PoolInfo& poolInfo = pools_[monitor][listId];

        // Log pool state before cleanup
        LOG_DEBUG("VideoPool", "Pool state before cleanup - Active: " +
            std::to_string(poolInfo.currentActive.load()) +
            ", Instances: " + std::to_string(poolInfo.instances.size()));

        // Clear all instances
        poolInfo.instances.clear();

        // Reset pool state
        poolInfo.poolInitialized.store(false);
        poolInfo.hasExtraInstance.store(false);
        poolInfo.currentActive.store(0);
        poolInfo.observedMaxActive.store(0);  // Reset observed maximum

        // Remove from maps
        pools_[monitor].erase(listId);
        if (pools_[monitor].empty()) {
            pools_.erase(monitor);
        }

        LOG_DEBUG("VideoPool", "Completed cleanup for List ID: " + std::to_string(listId));
    }
}

void VideoPool::shutdown() {
    std::unique_lock mapLock(mapMutex_);

    for (auto& [monitor, listPools] : pools_) {
        for (auto& [listId, poolInfo] : listPools) {
            if (!poolInfo.poolMutex.try_lock()) {
                LOG_WARNING("VideoPool", "Skipping busy pool during shutdown...");
                continue;
            }
            std::lock_guard poolLock(poolInfo.poolMutex, std::adopt_lock);

            // instances will clear automatically due to unique_ptr
            poolInfo.instances.clear();
            poolInfo.currentActive.store(0);
            poolInfo.poolInitialized.store(false);
            poolInfo.hasExtraInstance.store(false);
        }
        listPools.clear();
    }
    pools_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete");
}

void VideoPool::destroyVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId) {
    if (!vid) return;

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Skip if we can't get a lock - we'll fix it later
    if (!poolInfo->poolMutex.try_lock_for(std::chrono::milliseconds(100))) {
        // Decrement the counter even if we can't lock
        poolInfo->currentActive.fetch_sub(1);
        LOG_DEBUG("VideoPool", "Destroyed video instance without lock. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return;
    }

    std::unique_lock poolLock(poolInfo->poolMutex, std::adopt_lock);

    // Decrement active count
    poolInfo->currentActive.fetch_sub(1);

    // Determine if we need to replace this instance in the pool
    if (poolInfo->poolInitialized.load()) {
        // Calculate target pool size (observed max + 1)
        size_t targetTotal = poolInfo->observedMaxActive.load() + 1;

        // Calculate current total (active + pooled)
        size_t currentActive = poolInfo->currentActive.load();
        size_t currentPooled = poolInfo->instances.size();
        size_t currentTotal = currentActive + currentPooled;

        // If we're now below target, create a replacement
        if (currentTotal < targetTotal) {
            LOG_DEBUG("VideoPool", "Creating replacement for destroyed instance. Monitor: " +
                std::to_string(monitor) + ", List ID: " + std::to_string(listId));

            // Add a fresh instance to the pool
            poolInfo->instances.push_back(std::make_unique<GStreamerVideo>(monitor));

            // Notify any waiting threads
            poolInfo->waitCondition.notify_one();
        }
    }

    LOG_DEBUG("VideoPool", "Destroyed faulty video instance. Monitor: " +
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));

    // The unique_ptr will be destroyed when it goes out of scope
}

void VideoPool::trimExcessInstances(int monitor, int listId) {
    if (listId == -1) return;

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Try to lock with timeout - if can't get lock, just skip trimming
    if (!poolInfo->poolMutex.try_lock_for(std::chrono::milliseconds(100))) {
        return;
    }

    std::unique_lock poolLock(poolInfo->poolMutex, std::adopt_lock);

    // Don't trim if there are active users waiting
    if (poolInfo->waitCondition.wait_for(poolLock, std::chrono::milliseconds(0),
        []() { return true; }) == false) {
        return;
    }

    // Get the current active count and update the observed maximum
    size_t currentActive = poolInfo->currentActive.load();
    size_t observedMax = poolInfo->observedMaxActive.load();

    if (currentActive > observedMax) {
        poolInfo->observedMaxActive.store(currentActive);
        observedMax = currentActive;
    }

    // Target size = observed maximum + 1 extra instance (for safety)
    // But never go below 2 instances minimum
    size_t targetSize = std::max(observedMax + 1, size_t(2));

    // Keep the pool size reasonable
    size_t currentPoolSize = poolInfo->instances.size();

    // Only trim if we have substantially more instances than needed
    // This prevents constant resizing for small fluctuations
    if (currentPoolSize > targetSize + 2) {
        size_t excessCount = currentPoolSize - targetSize;
        LOG_DEBUG("VideoPool", "Trimming " + std::to_string(excessCount) +
            " excess instances (keeping " + std::to_string(targetSize) +
            ") for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));

        while (poolInfo->instances.size() > targetSize) {
            poolInfo->instances.pop_back();  // Remove oldest instances first
        }
    }
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
