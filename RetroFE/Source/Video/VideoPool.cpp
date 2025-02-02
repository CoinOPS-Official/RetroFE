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

    if (!poolInfo->poolMutex.try_lock()) {
        LOG_DEBUG("VideoPool", "Pool busy, creating new instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return new GStreamerVideo(monitor);
    }

    std::lock_guard<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);

    // Case 1: First time
    if (!poolInfo->poolInitialized.load()) {
        poolInfo->currentActive.fetch_add(1);
        LOG_DEBUG("VideoPool", "Creating first instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return new GStreamerVideo(monitor);
    }

    // Case 2: After first release, we WANT to create a new instance
    // instead of using the pooled one
    if (!poolInfo->hasExtraInstance.load()) {
        poolInfo->hasExtraInstance.store(true);
        poolInfo->currentActive.fetch_add(1);
        LOG_DEBUG("VideoPool", "Creating extra instance after first release. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return new GStreamerVideo(monitor);
    }

    // Case 3: Normal pool operation - use from pool if available
    if (!poolInfo->instances.empty()) {
        GStreamerVideo* vid = poolInfo->instances.front();
        poolInfo->instances.pop_front();
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

    // Case 4: Pool is empty, create new instance
    poolInfo->currentActive.fetch_add(1);
    LOG_DEBUG("VideoPool", "Creating new instance (pool empty). Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    if (listId == -1) {
        vid->stop();
        delete vid;
        return;
    }

    vid->unload();  // Unload before any locks

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    if (!poolInfo->poolMutex.try_lock()) {
        LOG_DEBUG("VideoPool", "Pool busy during release, deleting instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        vid->stop();
        delete vid;
        poolInfo->currentActive.fetch_sub(1);
        return;
    }

    std::lock_guard<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);

    // First, decrement active count
    size_t newActive = poolInfo->currentActive.fetch_sub(1) - 1;

    if (!poolInfo->poolInitialized.load()) {
        poolInfo->poolInitialized.store(true);
        poolInfo->instances.push_back(vid);
        LOG_DEBUG("VideoPool", "First release detected for Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return;
    }

    // Always add to pool - we want to maintain rotation
    poolInfo->instances.push_back(vid);
    LOG_DEBUG("VideoPool", "Instance added to pool. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Active: " + std::to_string(newActive) +
        ", Available: " + std::to_string(poolInfo->instances.size()));
}

void VideoPool::destroyVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    if (poolInfo) {
        // Decrement active count since we're destroying an instance
        poolInfo->currentActive.fetch_sub(1);
    }

    // Stop and destroy the video instance
    vid->stop();
    delete vid;

    LOG_DEBUG("VideoPool", "Destroyed faulty video instance. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::cleanup(int monitor, int listId) {
    if (listId == -1) return;

    std::unique_lock<std::shared_mutex> mapLock(mapMutex_);
    if (!pools_.count(monitor) || !pools_[monitor].count(listId)) {
        return;
    }

    PoolInfo& poolInfo = pools_[monitor][listId];

    // Check if there are still active videos
    size_t activeCount = poolInfo.currentActive.load();
    if (activeCount > 0 || poolInfo.instances.size() > 0) {
        LOG_WARNING("VideoPool", "Attempting cleanup with active/pooled videos. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
            ", Active: " + std::to_string(activeCount) +
            ", Pooled: " + std::to_string(poolInfo.instances.size()));
        return;
    }

    std::deque<GStreamerVideo*> toDelete;
    {
        if (!poolInfo.poolMutex.try_lock()) {
            LOG_DEBUG("VideoPool", "Pool busy during cleanup, deferring. Monitor: " + 
                std::to_string(monitor) + ", List ID: " + std::to_string(listId));
            return;
        }
        std::lock_guard<std::timed_mutex> poolLock(poolInfo.poolMutex, std::adopt_lock);

        // Clear all pool state
        toDelete.swap(poolInfo.instances);
        poolInfo.poolInitialized.store(false);
        poolInfo.hasExtraInstance.store(false);
        poolInfo.maxRequired.store(0);
        poolInfo.currentActive.store(0);
    }

    // Cleanup videos outside the locks
    size_t deletedCount = 0;
    for (auto* vid : toDelete) {
        if (vid) {  // Null check
            try {
                vid->stop();
                delete vid;
                deletedCount++;
            }
            catch (const std::exception& e) {
                LOG_ERROR("VideoPool", "Exception during video cleanup: " + std::string(e.what()));
            }
        }
    }
    toDelete.clear();  // Clear the temporary container

    // Remove pool from maps
    pools_[monitor].erase(listId);
    if (pools_[monitor].empty()) {
        pools_.erase(monitor);
    }

    LOG_DEBUG("VideoPool", "Pool cleaned up. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Videos deleted: " + std::to_string(deletedCount));
}
void VideoPool::shutdown() {
    std::unique_lock<std::shared_mutex> mapLock(mapMutex_);

    for (auto& [monitor, listPools] : pools_) {
        for (auto& [listId, poolInfo] : listPools) {
            if (!poolInfo.poolMutex.try_lock()) {
                // Instead of skipping, we should probably:
                // 1. Log the skip
                // 2. Possibly retry with timeout
                LOG_WARNING("VideoPool", "Skipping busy pool during shutdown. Monitor: " + 
                    std::to_string(monitor) + ", List ID: " + std::to_string(listId));
                continue;
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