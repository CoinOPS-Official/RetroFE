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

    if (!poolInfo->poolMutex.try_lock()) {
        LOG_DEBUG("VideoPool", "Pool busy, creating new instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return std::make_unique<GStreamerVideo>(monitor);
    }

    std::lock_guard<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);

    if (!poolInfo->poolInitialized.load()) {
        poolInfo->currentActive.fetch_add(1);
        LOG_DEBUG("VideoPool", "Creating first instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return std::make_unique<GStreamerVideo>(monitor);
    }

    if (!poolInfo->hasExtraInstance.load()) {
        poolInfo->hasExtraInstance.store(true);
        poolInfo->currentActive.fetch_add(1);
        LOG_DEBUG("VideoPool", "Creating extra instance after first release. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return std::make_unique<GStreamerVideo>(monitor);
    }

    if (!poolInfo->instances.empty()) {
        auto vid = std::move(poolInfo->instances.front());
        poolInfo->instances.pop_front();
        vid->setSoftOverlay(softOverlay);

        poolInfo->currentActive.fetch_add(1);
        size_t current = poolInfo->currentActive.load();
        size_t maxReq = poolInfo->maxRequired.load();
        while (maxReq < current && 
            !poolInfo->maxRequired.compare_exchange_weak(maxReq, current)) {}

        LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return vid;  // Implicit conversion from unique_ptr<GStreamerVideo> to unique_ptr<IVideo>
    }

    poolInfo->currentActive.fetch_add(1);
    return std::make_unique<GStreamerVideo>(monitor);
}

void VideoPool::releaseVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId) {
    if (!vid || listId == -1) return;

    vid->unload();

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    if (!poolInfo->poolMutex.try_lock()) {
        LOG_DEBUG("VideoPool", "Pool busy during release, deleting instance. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        // No need for explicit delete - unique_ptr will handle it
        poolInfo->currentActive.fetch_sub(1);
        return;
    }

    std::lock_guard<std::timed_mutex> poolLock(poolInfo->poolMutex, std::adopt_lock);
    size_t newActive = poolInfo->currentActive.fetch_sub(1) - 1;

    if (!poolInfo->poolInitialized.load()) {
        poolInfo->poolInitialized.store(true);
        poolInfo->instances.push_back(std::move(vid));
        LOG_DEBUG("VideoPool", "First release detected for Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
        return;
    }

    poolInfo->instances.push_back(std::move(vid));
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
        poolInfo.maxRequired.store(0);
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
            poolInfo.maxRequired.store(0);
        }
        listPools.clear();
    }
    pools_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete");
}

void VideoPool::destroyVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    PoolInfo* poolInfo = getPoolInfo(monitor, listId);

    if (poolInfo) {
        // Decrement active count since we're destroying an instance
        poolInfo->currentActive.fetch_sub(1);
    }

    // Destroy the video instance
    delete vid;

    LOG_DEBUG("VideoPool", "Destroyed faulty video instance. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}
