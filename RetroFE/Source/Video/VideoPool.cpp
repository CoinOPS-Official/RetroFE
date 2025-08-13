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
    // Non-pooled path
    if (listId == -1) {
        VideoPtr vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) {
            LOG_ERROR("VideoPool", "Failed to construct GStreamerVideo (non-pooled).");
            return nullptr;
        }
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    PoolInfo* poolPtr = nullptr;
    {
        std::lock_guard<std::mutex> globalLock(s_poolsMutex);
        poolPtr = &pools_[monitor][listId]; // auto-create if missing
    }

    VideoPtr vid;
    bool shouldCreateNew = false;

    for (;;) {
        // -------- decision block (locked) --------
        {
            std::lock_guard<std::mutex> lock(poolPtr->poolMutex);

            // PRE-LATCH PHASE: measure peak concurrency by creating on every acquire
            if (!poolPtr->initialCountLatched) {
                poolPtr->currentActive++;
                if (poolPtr->currentActive > poolPtr->observedMaxActive) {
                    poolPtr->observedMaxActive = poolPtr->currentActive;
                }
                shouldCreateNew = true;
                goto DECISION_MADE;
            }

            // POST-LATCH PHASE: GROWTH-FIRST UNTIL TARGET POPULATION REACHED
            const size_t totalPopulation =
                poolPtr->currentActive + poolPtr->ready.size() + poolPtr->pending.size();

            if (totalPopulation < poolPtr->requiredInstanceCount) {
                // keep creating until population == requiredInstanceCount
                poolPtr->currentActive++;
                shouldCreateNew = true;
                goto DECISION_MADE;
            }

            // At/above required population: reuse priorities
            // (1) ready queue
            if (!poolPtr->ready.empty()) {
                vid = std::move(poolPtr->ready.front());
                poolPtr->ready.pop_front();
                poolPtr->currentActive++;
                goto DECISION_MADE;
            }

            // (2) pending where state is None
            for (auto it = poolPtr->pending.begin(); it != poolPtr->pending.end(); ++it) {
                if ((*it)->getActualState() == IVideo::VideoState::None) {
                    vid = std::move(*it);
                    poolPtr->pending.erase(it);
                    poolPtr->currentActive++;
                    goto DECISION_MADE;
                }
            }

            // (3) pool already at target population: must wait
        } // unlock

        // -------- active wait (pool at target, nothing reusable yet) --------
        LOG_DEBUG("VideoPool", "Pool full, waiting for an instance to become ready...");

        {
            std::unique_lock<std::mutex> lk(poolPtr->poolMutex);
            poolPtr->poolCond.wait_for(
                lk, std::chrono::milliseconds(5),
                [&] {
                    return shuttingDown_ ||
                        !poolPtr->ready.empty() ||
                        std::any_of(poolPtr->pending.begin(), poolPtr->pending.end(),
                            [](const VideoPtr& v) {
                                return v->getActualState() == IVideo::VideoState::None;
                            }) ||
                        poolPtr->markedForCleanup;
                });
        }

        if (shuttingDown_) return nullptr;

        // Optional: safely pump default GLib context without racing another thread
        if (GMainContext* ctx = g_main_context_default()) {
            if (g_main_context_acquire(ctx)) {
                while (g_main_context_pending(ctx)) {
                    g_main_context_iteration(ctx, false);
                }
                g_main_context_release(ctx);
            }
        }

        continue;

    DECISION_MADE:
        break;
    }

    // create new instance if requested by decision logic
    if (shouldCreateNew) {
        vid = std::make_unique<GStreamerVideo>(monitor);
        if (!vid || vid->hasError()) {
            LOG_ERROR("VideoPool", "Failed to construct a new GStreamerVideo instance.");
            // decrement currentActive we incremented in the decision block
            std::lock_guard<std::mutex> lock(poolPtr->poolMutex);
            if (poolPtr->currentActive > 0) poolPtr->currentActive--;
            return nullptr;
        }
    }

    if (vid) {
        vid->setSoftOverlay(softOverlay);
    }

    return vid;
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
    if (!vid || listId == -1 || shuttingDown_) return;

    PoolInfo* poolPtr = nullptr;
    {
        std::lock_guard<std::mutex> globalLock(s_poolsMutex);
        auto monitorIt = pools_.find(monitor);
        if (monitorIt == pools_.end()) return;
        auto listIt = monitorIt->second.find(listId);
        if (listIt == monitorIt->second.end()) return;
        poolPtr = &listIt->second;
    }

    // Fast, non-blocking instance teardown; it will transition to None soon
    vid->unload();

    bool latchedOnThisRelease = false;
    size_t latchedPoolSize = 0;

    {
        std::lock_guard<std::mutex> lock(poolPtr->poolMutex);

        if (poolPtr->currentActive > 0) {
            poolPtr->currentActive--;
        }

        // put the instance into pending; acquire() will either reclaim later
        // (once at/over target population) or keep growing first.
        poolPtr->pending.push_back(std::move(vid));

        // LATCH HERE on the first release
        if (!poolPtr->initialCountLatched) {
            poolPtr->requiredInstanceCount = poolPtr->observedMaxActive + POOL_BUFFER_INSTANCES; // "+2"
            poolPtr->initialCountLatched = true;
            latchedOnThisRelease = true;
            latchedPoolSize = poolPtr->requiredInstanceCount;
        }

        LOG_DEBUG("VideoPool",
            "Instance moved to pending. Active: " + std::to_string(poolPtr->currentActive) +
            ", Ready: " + std::to_string(poolPtr->ready.size()) +
            ", Pending: " + std::to_string(poolPtr->pending.size()) +
            (latchedOnThisRelease ? (" [Latched pool size: " + std::to_string(latchedPoolSize) + "]") : ""));

        // wake any waiter in acquireVideo()
        poolPtr->poolCond.notify_all();
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