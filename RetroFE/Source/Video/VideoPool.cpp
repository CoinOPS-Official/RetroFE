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
// VideoPool.cpp
#include "VideoPool.h"

// Static member initialization
VideoPool::PoolMap VideoPool::pools_;
std::shared_mutex VideoPool::mapMutex_;

VideoPool::PoolInfo* VideoPool::getPoolInfo(int monitor, int listId) {
    // Try read-only access first
    {
        std::shared_lock<std::shared_mutex> readLock(mapMutex_);
        if (pools_.count(monitor) && pools_[monitor].count(listId)) {
            return &pools_[monitor][listId];
        }
    }

    // Need to create new pool info, acquire write lock
    std::unique_lock<std::shared_mutex> writeLock(mapMutex_);
    return &pools_[monitor][listId];
}

std::unique_ptr<IVideo> VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        return std::make_unique<GStreamerVideo>(monitor);
    }

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Get lock with timeout
    if (!poolInfo->poolMutex.try_lock_for(std::chrono::milliseconds(100))) {
        poolInfo->poolMutex.lock();
    }

    std::unique_lock<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);

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

    // Wait for available instance using condition variable
    poolInfo->waitCondition.wait(poolLock, [poolInfo]() {
        return !poolInfo->instances.empty();
        });

    auto vid = std::move(poolInfo->instances.front());
    poolInfo->instances.pop_front();
    vid->setSoftOverlay(softOverlay);
    poolInfo->currentActive.fetch_add(1);

    LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    return vid;
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

    vid->unload();

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Block until we can lock the pool mutex.
    std::unique_lock<std::timed_mutex> poolLock(poolInfo->poolMutex);

    // Decrement the active count under lock so that the instance can be accounted for.
    poolInfo->currentActive.fetch_sub(1);

    // On the first release, initialize the pool.
    if (!poolInfo->poolInitialized.load()) {
        poolInfo->poolInitialized.store(true);
        poolInfo->instances.push_back(std::move(vid));
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

    std::unique_lock<std::shared_mutex> mapLock(mapMutex_);

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

        // Remove from maps
        pools_[monitor].erase(listId);
        if (pools_[monitor].empty()) {
            pools_.erase(monitor);
        }

        LOG_DEBUG("VideoPool", "Completed cleanup for List ID: " + std::to_string(listId));
    }
}


void VideoPool::shutdown() {
    std::unique_lock<std::shared_mutex> mapLock(mapMutex_);

    for (auto& [monitor, listPools] : pools_) {
        for (auto& [listId, poolInfo] : listPools) {
            if (!poolInfo.poolMutex.try_lock()) {
                LOG_WARNING("VideoPool", "Skipping busy pool during shutdown...");
                continue;
            }
            std::lock_guard<std::timed_mutex> poolLock(poolInfo.poolMutex, std::adopt_lock);

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

    if (poolInfo) {
        // Decrement active count since we're destroying an instance
        poolInfo->currentActive.fetch_sub(1);
    }

    // The unique_ptr will automatically destroy the video instance when it goes out of scope

    LOG_DEBUG("VideoPool", "Destroyed faulty video instance. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}
