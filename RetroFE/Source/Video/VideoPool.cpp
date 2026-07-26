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
#include "GStreamerVideo.h"
#include "IVideo.h"

#include <algorithm>
#include <chrono>

VideoPool::PoolMap VideoPool::pools_{};
bool VideoPool::shuttingDown_ = false;


VideoPool::VideoPtr VideoPool::createNewVideo(int monitor, bool softOverlay) {
    auto vid = std::make_shared<GStreamerVideo>(monitor);
    if (!vid || vid->hasError()) return nullptr;

    vid->setSoftOverlay(softOverlay);
    return vid;
}

void VideoPool::pumpDrainingToReady(PoolInfo& pool) {
    for (size_t i = 0; i < pool.draining.size(); ) {
        if (auto* gsv = static_cast<GStreamerVideo*>(pool.draining[i].get())) {
            if (gsv->isReadyForReuse()) {
                pool.ready.push_back(std::move(pool.draining[i]));
                // Fast O(1) removal: swap with the back and pop
                pool.draining[i] = std::move(pool.draining.back());
                pool.draining.pop_back();
                continue; // Do NOT increment 'i', re-evaluate the swapped element
            }
        }
        ++i;
    }
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (shuttingDown_) return nullptr;

    // Non-pooled (listId -1)
    if (listId == -1) return createNewVideo(monitor, softOverlay);

    PoolInfo& pool = pools_[monitor][listId];
    if (pool.markedForCleanup) {
        LOG_DEBUG("VideoPool", "Acquire (bail: cleanup) " + poolStateStr(monitor, listId, pool));
        return nullptr;
    }

    // 1. Passive Observation: Advance any finished videos to the ready queue
    pumpDrainingToReady(pool);

    // 2. Instant O(1) Hot-Path Reuse
    if (!pool.ready.empty()) {
        auto vid = std::move(pool.ready.back());
        pool.ready.pop_back();
        pool.currentActive++;
        vid->setSoftOverlay(softOverlay);
        LOG_DEBUG("VideoPool", "Acquire (Ready Reuse) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    // 3. Growth Logic
    size_t totalCached = pool.ready.size() + pool.draining.size();
    if (!pool.initialCountLatched || (pool.currentActive + totalCached < pool.requiredInstanceCount)) {
        pool.currentActive++;

        if (!pool.initialCountLatched) {
            pool.observedMaxActive = std::max(pool.observedMaxActive, pool.currentActive);
        }

        auto vid = createNewVideo(monitor, softOverlay);
        if (!vid) {
            pool.currentActive--; // Prevent permanent inflation on failure
            LOG_DEBUG("VideoPool", "Acquire (New create FAIL) " + poolStateStr(monitor, listId, pool));
            return nullptr; // Fail Fast
        }
        LOG_DEBUG("VideoPool", "Acquire (New instance created) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    // 4. STRICT FAIL FAST
    LOG_DEBUG("VideoPool", "Acquire (FAIL FAST - Pool saturated) " + poolStateStr(monitor, listId, pool));
    return nullptr;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid || shuttingDown_ || listId == -1) return;

    PoolInfo& pool = pools_[monitor][listId];

    if (pool.currentActive > 0) pool.currentActive--;

    // --- FIX: Allow pre-reset videos to destroy themselves completely ---
    if (pool.purgeOnReleaseCount > 0) {
        pool.purgeOnReleaseCount--;
        LOG_DEBUG("VideoPool", "Release (Purging video for GStreamer cleanup) " + poolStateStr(monitor, listId, pool));
        return; // Early exit: reference drops to 0 and the pipeline is instantly destroyed
    }

    // Eviction ceiling — destroy without unloading, stop() handles cleanup
    if (pool.initialCountLatched) {
        size_t totalCached = pool.ready.size() + pool.draining.size();
        if (totalCached >= pool.requiredInstanceCount) {
            LOG_DEBUG("VideoPool", "Release (Evicting excess video) " + poolStateStr(monitor, listId, pool));
            return;
        }
    }

    if (pool.markedForCleanup) {
        if (pool.currentActive == 0) erasePoolIfIdle(monitor, listId);
        return;
    }

    // Only unload videos that are actually being returned to the pool
    if (auto* gsv = static_cast<GStreamerVideo*>(vid.get())) {
        gsv->unload();
    }

    pool.draining.push_back(std::move(vid));

    if (!pool.initialCountLatched) {
        pool.requiredInstanceCount = std::max(
            pool.requiredInstanceCount,
            pool.observedMaxActive + POOL_BUFFER_INSTANCES
        );
        pool.initialCountLatched = true;
        LOG_DEBUG("VideoPool", "Release (LATCHED cap=" + std::to_string(pool.requiredInstanceCount) + ") " + poolStateStr(monitor, listId, pool));
    }
    else {
        LOG_DEBUG("VideoPool", "Release (Cache to draining) " + poolStateStr(monitor, listId, pool));
    }
}

void VideoPool::erasePoolIfIdle(int monitor, int listId) {
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    auto& pool = lit->second;
    if (!pool.markedForCleanup || pool.currentActive != 0) return;

    pool.ready.clear();
    pool.draining.clear();
    mit->second.erase(lit);

    if (mit->second.empty()) pools_.erase(mit);
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr>& videos, int monitor, int listId) {
    if (shuttingDown_ || listId == -1) return;
    for (auto& vid : videos) {
        releaseVideo(std::move(vid), monitor, listId);
    }
    videos.clear(); // Clear the caller's vector since we moved the contents
}

void VideoPool::cleanup(int monitor, int listId) {
    if (shuttingDown_) return;

    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    lit->second.markedForCleanup = true;
    erasePoolIfIdle(monitor, listId);

    LOG_DEBUG("VideoPool", "Marked for cleanup: Mon:" + std::to_string(monitor) + " List:" + std::to_string(listId));
}

void VideoPool::decrementActive(int monitor, int listId) {
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;
    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;
    if (lit->second.currentActive > 0) lit->second.currentActive--;
}

void VideoPool::shutdown() {
    LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
    shuttingDown_ = true;
    pools_.clear();
    LOG_INFO("VideoPool", "VideoPool shutdown complete.");
}

std::string VideoPool::poolStateStr(int monitor, int listId, const PoolInfo& p) {
    return "Mon:" + std::to_string(monitor) +
        " List:" + std::to_string(listId) +
        " Active=" + std::to_string(p.currentActive) +
        " Ready=" + std::to_string(p.ready.size()) +
        " Draining=" + std::to_string(p.draining.size()) +
        " Req=" + std::to_string(p.requiredInstanceCount) +
        (p.initialCountLatched ? " LATCHED" : " PRELATCH");
}

void VideoPool::reserveCapacity(int monitor, int listId, size_t desiredTotal) {
    if (shuttingDown_) return;
    PoolInfo& pool = pools_[monitor][listId];
    pool.requiredInstanceCount = std::max(pool.requiredInstanceCount, desiredTotal);
}

void VideoPool::reset(int monitor, int listId) {
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    LOG_INFO("VideoPool", "Resetting pool cache for Mon:" + std::to_string(monitor)
        + " List:" + std::to_string(listId));

    auto& pool = lit->second;

    pool.ready.clear();
    pool.draining.clear();

    // Mark all currently active videos to be completely destroyed when released
    pool.purgeOnReleaseCount = pool.currentActive;
}
