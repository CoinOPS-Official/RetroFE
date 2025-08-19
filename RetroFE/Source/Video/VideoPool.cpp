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
#include "../Utility/ThreadPool.h"
#include <algorithm>
#include <memory>
#include <deque>
#include <unordered_map>

std::mutex VideoPool::s_poolsMutex;

namespace {
    constexpr size_t POOL_BUFFER_INSTANCES = 2;
    constexpr size_t HEALTH_CHECK_ACTIVE_THRESHOLD = 20;
    constexpr int HEALTH_CHECK_INTERVAL = 30;
}

VideoPool::PoolMap VideoPool::pools_;

std::atomic<bool> VideoPool::shuttingDown_ = false;

VideoPool::PoolInfo& VideoPool::getPoolInfo(int monitor, int listId) {
    return pools_[monitor][listId]; // will auto-create if not found
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    // ---- Non-pooled path ----
    if (listId == -1) {
        VideoPtr vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) {
            LOG_ERROR("VideoPool", "Failed to construct GStreamerVideo (non-pooled).");
            return nullptr;
        }
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) gsv->disarmOnBecameNone();
        vid->setSoftOverlay(softOverlay);
        LOG_DEBUG("VideoPool", "Acquire[-1:-1] Non-pooled new instance.");
        return vid;
    }

    // Locate/create pool entry
    PoolInfo* poolPtr = nullptr;
    {
        std::lock_guard<std::mutex> globalLock(s_poolsMutex);
        poolPtr = &pools_[monitor][listId]; // auto-creates
    }

    VideoPtr vid;
    bool shouldCreateNew = false;
    const char* modeStr = "Reuse"; // PreLatch / Growth / Reuse (for logging)

    for (;;) {
        // -------- Decision block (locked) --------
        {
            std::lock_guard<std::mutex> lock(poolPtr->poolMutex);

            // PRE-LATCH: always create, track peak
            if (!poolPtr->initialCountLatched) {
                poolPtr->currentActive++;
                poolPtr->observedMaxActive = std::max(poolPtr->observedMaxActive, poolPtr->currentActive);
                shouldCreateNew = true;
                modeStr = "PreLatch";
                goto DECISION_MADE;
            }

            // POST-LATCH: grow to target
            const size_t totalPopulation = poolPtr->currentActive + poolPtr->available.size();
            if (totalPopulation < poolPtr->requiredInstanceCount) {
                poolPtr->currentActive++;
                shouldCreateNew = true;
                modeStr = "Growth";
                goto DECISION_MADE;
            }

            // FAST PATH: consume known-ready hints in O(1)
            while (!poolPtr->readyHints.empty()) {
                IVideo* key = poolPtr->readyHints.front();
                poolPtr->readyHints.pop_front();

                auto idxIt = poolPtr->index.find(key);
                if (idxIt == poolPtr->index.end()) {
                    poolPtr->hinted.erase(key); // stale hint
                    continue;
                }

                auto it = idxIt->second; // iterator into available
                vid = std::move(*it);
                poolPtr->available.erase(it);
                poolPtr->index.erase(idxIt);
                poolPtr->hinted.erase(key);
                poolPtr->currentActive++;
                modeStr = "Reuse";
                goto DECISION_MADE;
            }

            // SLOW PATH (rare): oldest that has reached None
            if (!poolPtr->available.empty()) {
                for (auto it = poolPtr->available.begin(); it != poolPtr->available.end(); ++it) {
                    if ((*it)->getActualState() == IVideo::VideoState::None) {
                        IVideo* key = it->get();
                        vid = std::move(*it);
                        poolPtr->available.erase(it);
                        poolPtr->index.erase(key);
                        poolPtr->hinted.erase(key);
                        poolPtr->currentActive++;
                        modeStr = "Reuse";
                        goto DECISION_MADE;
                    }
                }
            }

            // Nothing ready ? wait
            LOG_DEBUG("VideoPool",
                "Acquire[" + std::to_string(monitor) + ":" + std::to_string(listId) + "] Waiting"
                " -> Active=" + std::to_string(poolPtr->currentActive) +
                ", Avail=" + std::to_string(poolPtr->available.size()) +
                ", Hints=" + std::to_string(poolPtr->readyHints.size()) +
                ", Target=" + std::to_string(poolPtr->requiredInstanceCount));
        } // unlock

        // Wait to be nudged by on-ready callback or periodic check
        {
            std::unique_lock<std::mutex> lk(poolPtr->poolMutex);
            poolPtr->poolCond.wait_for(
                lk, std::chrono::milliseconds(5),
                [&] {
                    if (shuttingDown_ || poolPtr->markedForCleanup) return true;
                    return !poolPtr->readyHints.empty() ||
                        std::any_of(poolPtr->available.begin(), poolPtr->available.end(),
                            [](const VideoPtr& v) { return v->getActualState() == IVideo::VideoState::None; });
                });
        }

        if (shuttingDown_) {
            LOG_DEBUG("VideoPool", "Acquire[" + std::to_string(monitor) + ":" + std::to_string(listId) + "] Aborted (shutdown).");
            return nullptr;
        }

        // (Optional) Nudge GLib teardown
        if (GMainContext* ctx = g_main_context_default()) {
            if (g_main_context_acquire(ctx)) {
                while (g_main_context_pending(ctx)) g_main_context_iteration(ctx, false);
                g_main_context_release(ctx);
            }
        }
        continue;

    DECISION_MADE:
        // single condensed acquire log (under lock state)
        LOG_DEBUG("VideoPool",
            std::string("Acquire[") + std::to_string(monitor) + ":" + std::to_string(listId) + "] " + modeStr +
            " -> Active=" + std::to_string(poolPtr->currentActive) +
            ", Avail=" + std::to_string(poolPtr->available.size()) +
            ", Hints=" + std::to_string(poolPtr->readyHints.size()) +
            ", Target=" + std::to_string(poolPtr->requiredInstanceCount));
        break;
    }

    // Create if requested
    if (shouldCreateNew) {
        vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) {
            LOG_ERROR("VideoPool", "Failed to construct a new GStreamerVideo instance.");
            std::lock_guard<std::mutex> lock(poolPtr->poolMutex);
            if (poolPtr->currentActive > 0) poolPtr->currentActive--;
            return nullptr;
        }
    }

    // Instance becomes active
    if (vid) {
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) gsv->disarmOnBecameNone();
        vid->setSoftOverlay(softOverlay);
    }

    return vid;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid || listId == -1 || shuttingDown_) return;

    // Resolve pool under global lock (unchanged)
    PoolInfo* poolPtr = nullptr;
    {
        std::lock_guard<std::mutex> globalLock(s_poolsMutex);
        auto monitorIt = pools_.find(monitor);
        if (monitorIt == pools_.end()) return;
        auto listIt = monitorIt->second.find(listId);
        if (listIt == monitorIt->second.end()) return;
        poolPtr = &listIt->second;
    }

    IVideo* key = vid.get();
    bool latchedNow = false;
    size_t latchedPoolSize = 0;

    {
        std::lock_guard<std::mutex> lock(poolPtr->poolMutex);

        if (poolPtr->currentActive > 0) poolPtr->currentActive--;

        // Insert into idle list and index it (oldest-first)
        auto it = poolPtr->available.insert(poolPtr->available.end(), std::move(vid));
        poolPtr->index[key] = it;

        // Latch on first release
        if (!poolPtr->initialCountLatched) {
            poolPtr->requiredInstanceCount = poolPtr->observedMaxActive + POOL_BUFFER_INSTANCES;
            poolPtr->initialCountLatched = true;
            latchedNow = true;
            latchedPoolSize = poolPtr->requiredInstanceCount;

            LOG_INFO("VideoPool",
                     "Latched pool size: observed max " + std::to_string(poolPtr->observedMaxActive) +
                     " + buffer " + std::to_string(POOL_BUFFER_INSTANCES) +
                     " = " + std::to_string(poolPtr->requiredInstanceCount));
        }

        // Arm one-shot: when it hits None, queue a hint and wake waiters.
        // IMPORTANT: hold s_poolsMutex until AFTER we own pool->poolMutex,
        // so PoolInfo can't be destroyed while we lock/use it.
        if (auto* gsv = dynamic_cast<GStreamerVideo*>(it->get())) {
            gsv->armOnBecameNone([monitor, listId, key](GStreamerVideo* /*who*/) {
                std::unique_lock<std::mutex> glock(s_poolsMutex);
                auto mIt = pools_.find(monitor);
                if (mIt == pools_.end()) return;
                auto lIt = mIt->second.find(listId);
                if (lIt == mIt->second.end()) return;

                PoolInfo* pool = &lIt->second;

                // Take local lock while still holding global to guarantee lifetime
                std::lock_guard<std::mutex> plock(pool->poolMutex);

                if (pool->index.find(key) != pool->index.end() && !pool->hinted.count(key)) {
                    pool->readyHints.push_back(key);
                    pool->hinted.insert(key);
                }
                pool->poolCond.notify_all();
                // RAII releases plock, then glock
            });
        }

        LOG_DEBUG("VideoPool",
                  "Release[" + std::to_string(monitor) + ":" + std::to_string(listId) + "] " +
                  "Active=" + std::to_string(poolPtr->currentActive) +
                  ", Avail=" + std::to_string(poolPtr->available.size()) +
                  ", Hints=" + std::to_string(poolPtr->readyHints.size()) +
                  (latchedNow ? " [Latched=" + std::to_string(latchedPoolSize) + "]" : ""));
    }

    // Kick teardown OUTSIDE locks; callback will nudge waiters at None.
    if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) {
        gsv->unload();
    }
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
    if (videos.empty() || listId == -1 || shuttingDown_) return;
    for (auto& vid : videos) {
        releaseVideo(std::move(vid), monitor, listId);
    }
}

void VideoPool::cleanup_nolock(int monitor, int listId) {
    auto monitorIt = pools_.find(monitor);
    if (monitorIt == pools_.end()) return;
    auto listIt = monitorIt->second.find(listId);
    if (listIt == monitorIt->second.end()) return;

    PoolInfo& pool = listIt->second;
    {
        std::lock_guard<std::mutex> lock(pool.poolMutex);
        pool.markedForCleanup = true;
        pool.poolCond.notify_all();
    }
    LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) +
        ", List ID: " + std::to_string(listId));
}

void VideoPool::cleanup(int monitor, int listId) {
    std::lock_guard<std::mutex> globalLock(s_poolsMutex);
    auto monitorIt = pools_.find(monitor);
    if (monitorIt == pools_.end()) return;
    auto listIt = monitorIt->second.find(listId);
    if (listIt == monitorIt->second.end()) return;

    PoolInfo& pool = listIt->second;
    {
        std::lock_guard<std::mutex> lock(pool.poolMutex);
        pool.markedForCleanup = true;
        pool.poolCond.notify_all();
    }
    LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) +
        ", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
    LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
    shuttingDown_ = true;

    {
        std::lock_guard<std::mutex> globalLock(s_poolsMutex);
        for (auto& [monitor, listMap] : pools_) {
            for (auto& [listId, pool] : listMap) {
                std::lock_guard<std::mutex> localLock(pool.poolMutex);
                pool.markedForCleanup = true;
                pool.poolCond.notify_all();
            }
        }
    }

    // Destroy all pools and their instances
    {
        std::lock_guard<std::mutex> globalLock(s_poolsMutex);
        pools_.clear();
    }

    shuttingDown_ = false;
    LOG_INFO("VideoPool", "VideoPool shutdown complete.");
}

bool VideoPool::checkPoolHealth(int monitor, int listId) {
    // globalLock is already held by acquireVideo when this is called.
    auto monitorIt = pools_.find(monitor);
    if (monitorIt == pools_.end()) return true;
    auto listIt = monitorIt->second.find(listId);
    if (listIt == monitorIt->second.end()) return true;

    PoolInfo& pool = listIt->second;
    {
        // FIX: Acquire poolMutex to safely read currentActive
        std::lock_guard<std::mutex> lock(pool.poolMutex);
        if (pool.currentActive > HEALTH_CHECK_ACTIVE_THRESHOLD) {
            LOG_WARNING("VideoPool", "Health check: suspicious active count: " + std::to_string(pool.currentActive) +
                " for Monitor: " + std::to_string(monitor) + ", List ID: " + std::to_string(listId));
            return false;
        }
    } // lock released
    return true;
}
