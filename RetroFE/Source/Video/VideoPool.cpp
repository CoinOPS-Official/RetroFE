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
#ifdef RETROFE_HAVE_FFMPEG
#include "FFmpegVideo.h"
#endif
#include "VideoFactory.h"

#include <algorithm>
#include <chrono>

VideoPool::PoolMap VideoPool::pools_{};
bool VideoPool::shuttingDown_ = false;


VideoPool::VideoPtr VideoPool::createNewVideo(int monitor, bool softOverlay) {
    VideoPtr vid;
#ifdef RETROFE_HAVE_FFMPEG
    if (VideoFactory::backend() == "ffmpeg") vid = std::make_shared<FFmpegVideo>(monitor);
    else
#endif
        vid = std::make_shared<GStreamerVideo>(monitor);
    if (!vid || vid->hasError()) return nullptr;

    vid->setSoftOverlay(softOverlay);
    return vid;
}

void VideoPool::pumpDrainingToReady(PoolInfo& pool) {
    for (size_t i = 0; i < pool.draining.size(); ) {
        if (auto* gsv = pool.draining[i].get()) {
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

    if (listId == -1) return createNewVideo(monitor, softOverlay);

    PoolInfo& pool = pools_[monitor][listId];
    if (pool.markedForCleanup) {
        LOG_DEBUG("VideoPool", "Acquire (bail: cleanup) " + poolStateStr(monitor, listId, pool));
        return nullptr;
    }

    pumpDrainingToReady(pool);

    if (!pool.ready.empty()) {
        auto vid = std::move(pool.ready.back());
        pool.ready.pop_back();

        pool.currentActive++;
        pool.activeVideos.insert(vid.get());

        vid->setSoftOverlay(softOverlay);
        LOG_DEBUG("VideoPool", "Acquire (Ready Reuse) " + poolStateStr(monitor, listId, pool));
        return vid;
    }

    const size_t totalCached =
        pool.ready.size() + pool.draining.size();

    const size_t totalOwned =
        pool.currentActive + totalCached;

    auto createTracked =
        [&](const char* reason, bool observeSteadyDemand) -> VideoPtr
    {
        auto vid = createNewVideo(monitor, softOverlay);
        if (!vid) {
            LOG_DEBUG(
                "VideoPool",
                "Acquire (New create FAIL) " +
                poolStateStr(monitor, listId, pool));
            return nullptr;
        }

        pool.currentActive++;
        pool.activeVideos.insert(vid.get());

        // Only the original pre-latch discovery phase teaches us the steady
        // layout demand. Elastic burst allocations must never permanently grow
        // the steady cache target.
        if (observeSteadyDemand && !pool.initialCountLatched) {
            pool.observedMaxActive =
                std::max(pool.observedMaxActive, pool.currentActive);
        }

        LOG_DEBUG(
            "VideoPool",
            std::string(reason) + " " +
            poolStateStr(monitor, listId, pool));

        return vid;
    };

    // Preserve the original discovery behavior until the first release latches
    // the steady target. After latching, fill only up to requiredInstanceCount
    // as ordinary capacity.
    if (!pool.initialCountLatched ||
        totalOwned < pool.requiredInstanceCount)
    {
        return createTracked(
            "Acquire (New instance created)",
            true);
    }

    // Once steady capacity is exhausted, allow a small bounded burst above it.
    // GStreamerVideo independently limits expensive URI/decode transitions, so
    // these extra objects mostly absorb scheduler waiters and draining overlap.
    const size_t elasticLimit =
        pool.requiredInstanceCount + POOL_ELASTIC_INSTANCES;

    if (totalOwned < elasticLimit) {
        return createTracked(
            "Acquire (ELASTIC instance created)",
            false);
    }

    LOG_DEBUG(
        "VideoPool",
        "Acquire (FAIL FAST - Elastic limit saturated) " +
        poolStateStr(monitor, listId, pool));

    return nullptr;
}

VideoPool::VideoPtr VideoPool::acquireWarmForReset(
    int monitor,
    int listId,
    bool softOverlay)
{
    if (shuttingDown_ || listId == -1) return nullptr;

    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return nullptr;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return nullptr;

    PoolInfo& pool = lit->second;
    if (pool.markedForCleanup) return nullptr;

    // Non-blocking: only completed drains move to READY.
    pumpDrainingToReady(pool);

    if (pool.ready.empty()) {
        LOG_DEBUG("VideoPool",
            "Playlist handoff: no warm cached instance " +
            poolStateStr(monitor, listId, pool));
        return nullptr;
    }

    auto vid = std::move(pool.ready.back());
    pool.ready.pop_back();

    pool.currentActive++;
    pool.activeVideos.insert(vid.get());

    vid->setSoftOverlay(softOverlay);

    LOG_DEBUG("VideoPool",
        "Playlist handoff: claimed warm cached instance " +
        poolStateStr(monitor, listId, pool));

    return vid;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid || shuttingDown_ || listId == -1) return;

    PoolInfo& pool = pools_[monitor][listId];
    IVideo* raw = vid.get();

    if (pool.activeVideos.erase(raw) > 0) {
        if (pool.currentActive > 0) pool.currentActive--;
    }
    else {
        LOG_DEBUG("VideoPool",
            "Release (instance was not tracked active) " +
            poolStateStr(monitor, listId, pool));
    }

    // Old-generation instances may finish their current ownership, but must
    // never re-enter draining/ready. The temporary playlist handoff is covered
    // by this same rule and dies on its first release after the reset.
    if (pool.retireOnRelease.erase(raw) > 0) {
        LOG_DEBUG("VideoPool",
            "Release (Retiring pre-reset video for GStreamer cleanup) " +
            poolStateStr(monitor, listId, pool));
        return;
    }

    if (pool.initialCountLatched) {
        const size_t totalCached = pool.ready.size() + pool.draining.size();
        if (totalCached >= pool.requiredInstanceCount) {
            LOG_DEBUG("VideoPool",
                "Release (Evicting excess/elastic video) " +
                poolStateStr(monitor, listId, pool));
            return;
        }
    }

    if (pool.markedForCleanup) {
        if (pool.currentActive == 0) erasePoolIfIdle(monitor, listId);
        return;
    }

    if (auto* gsv = vid.get()) {
        gsv->unload();
    }

    pool.draining.push_back(std::move(vid));

    if (!pool.initialCountLatched) {
        const size_t observedTarget =
            pool.observedMaxActive + POOL_BUFFER_INSTANCES;

        // reserveCapacity() may have supplied a larger minimum before the first
        // release. Never overwrite that explicit steady-capacity hint.
        pool.requiredInstanceCount =
            std::max(pool.requiredInstanceCount, observedTarget);

        pool.initialCountLatched = true;

        LOG_DEBUG("VideoPool",
            "Release (LATCHED steady=" +
            std::to_string(pool.requiredInstanceCount) +
            " elasticMax=" +
            std::to_string(pool.requiredInstanceCount + POOL_ELASTIC_INSTANCES) +
            ") " +
            poolStateStr(monitor, listId, pool));
    }
    else {
        LOG_DEBUG("VideoPool",
            "Release (Cache to draining) " +
            poolStateStr(monitor, listId, pool));
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
        " Retire=" + std::to_string(p.retireOnRelease.size()) +
        " Req=" + std::to_string(p.requiredInstanceCount) +
        " ElasticMax=" +
        std::to_string(
            p.initialCountLatched
            ? p.requiredInstanceCount + POOL_ELASTIC_INSTANCES
            : 0) +
        (p.initialCountLatched ? " LATCHED" : " PRELATCH");
}

void VideoPool::reserveCapacity(int monitor, int listId, size_t desiredTotal) {
    if (shuttingDown_) return;

    PoolInfo& pool = pools_[monitor][listId];

    const size_t oldRequired = pool.requiredInstanceCount;
    pool.requiredInstanceCount =
        std::max(pool.requiredInstanceCount, desiredTotal);

    if (pool.requiredInstanceCount != oldRequired) {
        LOG_DEBUG(
            "VideoPool",
            "Reserve raised steady capacity to " +
            std::to_string(pool.requiredInstanceCount) + " " +
            poolStateStr(monitor, listId, pool));
    }
}

void VideoPool::reset(int monitor, int listId) {
    auto mit = pools_.find(monitor);
    if (mit == pools_.end()) return;

    auto lit = mit->second.find(listId);
    if (lit == mit->second.end()) return;

    auto& pool = lit->second;

    LOG_INFO("VideoPool",
        "Resetting pool cache for Mon:" + std::to_string(monitor) +
        " List:" + std::to_string(listId) +
        " Active=" + std::to_string(pool.currentActive) +
        " Ready=" + std::to_string(pool.ready.size()) +
        " Draining=" + std::to_string(pool.draining.size()));

    // Hard hygiene boundary for cached state.
    // A warm handoff that should survive must be acquired BEFORE reset(),
    // which makes it active and therefore marks it for retirement below.
    pool.ready.clear();
    pool.draining.clear();

    // Every instance active at the boundary is old-generation. It may remain
    // owned temporarily, but its next release destroys it instead of caching it.
    pool.retireOnRelease.insert(
        pool.activeVideos.begin(),
        pool.activeVideos.end());

    LOG_DEBUG("VideoPool",
        "Reset armed retirement for " +
        std::to_string(pool.retireOnRelease.size()) +
        " active instance(s): " +
        poolStateStr(monitor, listId, pool));
}

