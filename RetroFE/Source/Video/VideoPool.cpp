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

// Define static member
VideoPool::PoolMap VideoPool::pools_;

GStreamerVideo* VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        return new GStreamerVideo(monitor);
    }

    auto& monitorMap = pools_[monitor];
    auto& poolInfo = monitorMap[listId];

    // Case 1: Initial instances or creating extra instance
    if (!poolInfo.poolInitialized || (poolInfo.poolInitialized && !poolInfo.hasExtraInstance)) {
        poolInfo.currentActive++;
        poolInfo.maxRequired = std::max(poolInfo.maxRequired, poolInfo.currentActive);

        // If this is the extra instance (creating after pool initialized)
        if (poolInfo.poolInitialized) {
            poolInfo.hasExtraInstance = true;
            LOG_DEBUG("VideoPool", "Creating extra instance. Monitor: " + 
                std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                ", Active: " + std::to_string(poolInfo.currentActive));
        } else {
            LOG_DEBUG("VideoPool", "Creating new instance. Monitor: " + 
                std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
                ", Active: " + std::to_string(poolInfo.currentActive));
        }
        return new GStreamerVideo(monitor);
    }

    // Case 2: After extra instance is created, use pool
    if (!poolInfo.instances.empty()) {
        GStreamerVideo* vid = poolInfo.instances.back();
        poolInfo.instances.pop_back();
        vid->setSoftOverlay(softOverlay);
        poolInfo.currentActive++;
        poolInfo.maxRequired = std::max(poolInfo.maxRequired, poolInfo.currentActive);

        LOG_DEBUG("VideoPool", "Reusing instance from pool. Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
            ", Active: " + std::to_string(poolInfo.currentActive) +
            ", Available: " + std::to_string(poolInfo.instances.size()));
        return vid;
    }

    // Case 3: Pool empty but initialized and has extra, create new
    poolInfo.currentActive++;
    poolInfo.maxRequired = std::max(poolInfo.maxRequired, poolInfo.currentActive);

    LOG_DEBUG("VideoPool", "Creating new instance (pool empty). Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Active: " + std::to_string(poolInfo.currentActive));
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    if (listId == -1) {
        vid->stop();
        delete vid;
        return;
    }

    auto& monitorMap = pools_[monitor];
    auto& poolInfo = monitorMap[listId];

    // Mark pool as initialized on first release
    if (!poolInfo.poolInitialized) {
        poolInfo.poolInitialized = true;
        LOG_DEBUG("VideoPool", "First release detected for Monitor: " + 
            std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    }

    vid->unload();
    poolInfo.instances.push_back(vid);
    poolInfo.currentActive--;

    LOG_DEBUG("VideoPool", "Released to pool. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Active: " + std::to_string(poolInfo.currentActive) +
        ", Available: " + std::to_string(poolInfo.instances.size()) +
        ", Max required: " + std::to_string(poolInfo.maxRequired));
}


void VideoPool::cleanup(int monitor, int listId) {
    if (listId == -1) return;

    auto& monitorMap = pools_[monitor];
    auto& poolInfo = monitorMap[listId];

    // Just stop and delete videos in pool - no need to unload
    for (auto* vid : poolInfo.instances) {
        vid->stop();  // Just stop
        delete vid;   // Then delete
    }

    // Clear bookkeeping
    poolInfo.instances.clear();
    poolInfo.currentActive = 0;
    poolInfo.poolInitialized = false;
    poolInfo.hasExtraInstance = false;
    poolInfo.maxRequired = 0;

    monitorMap.erase(listId);
    if (monitorMap.empty()) {
        pools_.erase(monitor);
    }

    LOG_DEBUG("VideoPool", "Pool cleaned up. Monitor: " + 
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
    for (auto& [monitor, listPools] : pools_) {
        for (auto& [listId, poolInfo] : listPools) {
            for (auto* vid : poolInfo.instances) {
                vid->stop();
                delete vid;
            }
            poolInfo.instances.clear();
            poolInfo.currentActive = 0;
            poolInfo.poolInitialized = false;
            poolInfo.maxRequired = 0;
        }
        listPools.clear();
    }
    pools_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete, all pooled videos destroyed.");
}