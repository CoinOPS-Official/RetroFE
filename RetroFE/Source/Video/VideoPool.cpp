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

GStreamerVideo* VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        return new GStreamerVideo(monitor);
    }

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Try to acquire pool lock - create new instance if pool is busy
    if (!poolInfo->poolMutex.try_lock()) {
        LOG_DEBUG("VideoPool", "Pool busy, creating new instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return new GStreamerVideo(monitor);
    }

    // Fix: Change std::mutex to std::timed_mutex in the lock_guard
    std::lock_guard<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);

    // Case 1: Initial instances or creating extra instance
    if (!poolInfo->poolInitialized.load() || 
        (poolInfo->poolInitialized.load() && !poolInfo->hasExtraInstance.load())) {

        poolInfo->currentActive.fetch_add(1);
        size_t current = poolInfo->currentActive.load();
        size_t maxReq = poolInfo->maxRequired.load();
        while (maxReq < current && 
            !poolInfo->maxRequired.compare_exchange_weak(maxReq, current)) {}

        if (poolInfo->poolInitialized.load()) {
            poolInfo->hasExtraInstance.store(true);
            LOG_DEBUG("VideoPool", "Creating extra instance. Monitor: " + 
                std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                ", Active: " + std::to_string(current));  // Added active count
        } else {
            LOG_DEBUG("VideoPool", "Creating new instance. Monitor: " + 
                std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                ", Active: " + std::to_string(current));  // Added active count
        }
        return new GStreamerVideo(monitor);
    }

    // Case 2: Reuse from pool
    if (!poolInfo->instances.empty()) {
        GStreamerVideo* vid = poolInfo->instances.back();
        poolInfo->instances.pop_back();
        vid->setSoftOverlay(softOverlay);

        poolInfo->currentActive.fetch_add(1);
        size_t current = poolInfo->currentActive.load();
        size_t maxReq = poolInfo->maxRequired.load();
        while (maxReq < current && 
            !poolInfo->maxRequired.compare_exchange_weak(maxReq, current)) {}

        LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
            ", Active: " + std::to_string(current) +
            ", Available: " + std::to_string(poolInfo->instances.size()));
        return vid;
    }

    // Case 3: Create new instance if pool is empty
    poolInfo->currentActive.fetch_add(1);
    size_t current = poolInfo->currentActive.load();
    LOG_DEBUG("VideoPool", "Creating new instance (pool empty). Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Active: " + std::to_string(current));
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    if (listId == -1) {
        vid->stop();
        delete vid;
        return;
    }

    vid->unload();  // Unload video before acquiring any locks

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    // Try to return to pool, delete if pool is busy
    if (!poolInfo->poolMutex.try_lock()) {
        LOG_DEBUG("VideoPool", "Pool busy during release, deleting instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        delete vid;
        poolInfo->currentActive.fetch_sub(1);
        return;
    }

    std::lock_guard<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);

    if (!poolInfo->poolInitialized.load()) {
        poolInfo->poolInitialized.store(true);
        LOG_DEBUG("VideoPool", "First release detected for Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    }

    poolInfo->instances.push_back(vid);
    size_t newActive = poolInfo->currentActive.fetch_sub(1) - 1;

    LOG_DEBUG("VideoPool", "Released to pool. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Active: " + std::to_string(newActive) +
        ", Available: " + std::to_string(poolInfo->instances.size()) +
        ", Max required: " + std::to_string(poolInfo->maxRequired.load()));
}

void VideoPool::cleanup(int monitor, int listId) {
    if (listId == -1) return;

    std::unique_lock<std::shared_mutex> mapLock(mapMutex_);
    if (!pools_.count(monitor) || !pools_[monitor].count(listId)) {
        return;
    }

    PoolInfo& poolInfo = pools_[monitor][listId];

    // Move instances to temporary vector for cleanup
    std::vector<GStreamerVideo*> toDelete;
    {
        if (!poolInfo.poolMutex.try_lock()) {
            LOG_DEBUG("VideoPool", "Pool busy during cleanup, deferring. Monitor: " + 
                std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                ", Active: " + std::to_string(poolInfo.currentActive.load()) +
                ", Available: " + std::to_string(poolInfo.instances.size()));
            return;
        }
        std::lock_guard<std::timed_mutex> poolLock(poolInfo.poolMutex, std::adopt_lock);
        toDelete.swap(poolInfo.instances);
    }

    // Cleanup outside the locks
    for (auto* vid : toDelete) {
        vid->stop();
        delete vid;
    }

    // Remove pool from maps
    pools_[monitor].erase(listId);
    if (pools_[monitor].empty()) {
        pools_.erase(monitor);
    }

    LOG_DEBUG("VideoPool", "Pool cleaned up. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
    std::unique_lock<std::shared_mutex> mapLock(mapMutex_);

    for (auto& [monitor, listPools] : pools_) {
        for (auto& [listId, poolInfo] : listPools) {
            if (!poolInfo.poolMutex.try_lock()) {  // Changed to try_lock()
                continue;  // Skip busy pools during shutdown
            }
            std::lock_guard<std::timed_mutex> poolLock(poolInfo.poolMutex, std::adopt_lock);  // Changed to timed_mutex

            for (auto* vid : poolInfo.instances) {
                vid->stop();
                delete vid;
            }
            poolInfo.instances.clear();
            poolInfo.currentActive.store(0);
            poolInfo.poolInitialized.store(false);
            poolInfo.hasExtraInstance.store(false);
            poolInfo.maxRequired.store(0);
        }
        listPools.clear();
    }
    pools_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete, all pooled videos destroyed.");
}