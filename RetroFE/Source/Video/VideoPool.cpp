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

std::mutex VideoPool::s_mutex;
std::condition_variable VideoPool::s_cv;
std::atomic<bool> VideoPool::shuttingDown_ = false;
VideoPool::PoolMap VideoPool::pools_;

namespace {
	constexpr size_t POOL_BUFFER_INSTANCES = 2;
}

VideoPool::VideoPtr VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
	if (listId == -1) {
		auto vid = std::make_unique<GStreamerVideo>(monitor);
		if (!vid || vid->hasError()) return nullptr; // non-pooled caller should handle
		if (auto* gsv = vid.get()) gsv->disarmOnBecameNone();
		vid->setSoftOverlay(softOverlay);
		return vid;
	}

	std::unique_lock<std::mutex> lk(s_mutex);
	PoolInfo& pool = pools_[monitor][listId];

	auto try_pop_ready = [&]() -> VideoPtr {
		// fast path: ready hint
		while (!pool.readyHints.empty()) {
			IVideo* key = pool.readyHints.front();
			pool.readyHints.pop_front();
			auto idx = pool.index.find(key);
			if (idx == pool.index.end()) continue; // stale
			auto it = idx->second;
			auto vid = std::move(*it);
			pool.available.erase(it);
			pool.index.erase(key);
			pool.hinted.erase(key);
			pool.currentActive++;
			LOG_DEBUG("VideoPool", "Acquire (readyHint reuse) " + poolStateStr(monitor, listId, pool));
			return vid;
		}
		// slow path: scan for None
		for (auto it = pool.available.begin(); it != pool.available.end(); ++it) {
			if ((*it)->getActualState() == IVideo::VideoState::None) {
				IVideo* key = it->get();
				auto vid = std::move(*it);
				pool.available.erase(it);
				pool.index.erase(key);
				pool.hinted.erase(key);
				pool.currentActive++;
				LOG_DEBUG("VideoPool", "Acquire (scan reuse) " + poolStateStr(monitor, listId, pool));
				return vid;
			}
		}
		return nullptr;
		};

	// PRE-LATCH: always create (records peak)
	if (!pool.initialCountLatched) {
		pool.currentActive++;
		pool.observedMaxActive = std::max(pool.observedMaxActive, pool.currentActive);
		lk.unlock();

		auto vid = std::make_unique<GStreamerVideo>(monitor);
		if (!vid || vid->hasError()) {

			pool.currentActive--;
			LOG_WARNING("VideoPool", "Acquire (PreLatch create FAIL) " + poolStateStr(monitor, listId, pool));

			s_cv.notify_all();
			lk.lock();
		}
		else {
			LOG_DEBUG("VideoPool", "Acquire (PreLatch create OK) " + poolStateStr(monitor, listId, pool));

			if (auto* gsv = vid.get()) gsv->disarmOnBecameNone();
			vid->setSoftOverlay(softOverlay);
			return vid;
		}
	}

	// Post-latch loop: block until we can return a video
	for (;;) {
		if (shuttingDown_ || pool.markedForCleanup)
		{
			LOG_DEBUG("VideoPool", "Acquire (bail: shutdown/cleanup) " + poolStateStr(monitor, listId, pool));
			return nullptr;
		}
		// 1) any ready?
		if (auto vid = try_pop_ready()) {
			LOG_DEBUG("VideoPool", "Acquire (reuse OK) " + poolStateStr(monitor, listId, pool));
			lk.unlock(); // unlock after logging
			if (auto* gsv = dynamic_cast<GStreamerVideo*>(vid.get())) gsv->disarmOnBecameNone();
			vid->setSoftOverlay(softOverlay);
			return vid;
		}

		// 2) allowed to grow?
		const size_t total = pool.currentActive + pool.available.size();
		if (total < pool.requiredInstanceCount) {
			pool.currentActive++;
			lk.unlock();

			auto vid = std::make_unique<GStreamerVideo>(monitor);
			if (!vid || vid->hasError()) {
				pool.currentActive--;
				s_cv.notify_all();
				lk.lock();
			}
			else {
				LOG_DEBUG("VideoPool", "Acquire (Growth create OK) " + poolStateStr(monitor, listId, pool));
				if (auto* gsv = vid.get()) gsv->disarmOnBecameNone();
				vid->setSoftOverlay(softOverlay);
				return vid;
			}
		}

		LOG_DEBUG("VideoPool", "Acquire (wait) " + poolStateStr(monitor, listId, pool));

		// 3) wait until: hint arrives, an idle reaches None, cleanup/shutdown, or growth becomes possible
		s_cv.wait(lk, [&pool] {
			if (shuttingDown_ || pool.markedForCleanup) return true;
			if (!pool.readyHints.empty()) return true;
			if (std::any_of(pool.available.begin(), pool.available.end(),
				[](const VideoPtr& v) { return v->getActualState() == IVideo::VideoState::None; })) return true;
			// growth may become possible after releases reduce total
			const size_t totalNow = pool.currentActive + pool.available.size();
			return totalNow < pool.requiredInstanceCount;
			});
		// loop re-checks everything
	}
}

void VideoPool::releaseVideo(VideoPtr vid, int monitor, int listId) {
	if (!vid || listId == -1 || shuttingDown_) return;

	std::unique_lock<std::mutex> lk(s_mutex);
	auto mit = pools_.find(monitor);
	if (mit == pools_.end()) return;
	auto lit = mit->second.find(listId);
	if (lit == mit->second.end()) return;
	PoolInfo& pool = lit->second;

	if (pool.currentActive > 0) pool.currentActive--;

	IVideo* key = vid.get();
	auto it = pool.available.insert(pool.available.end(), std::move(vid));
	pool.index[key] = it;

	if (!pool.initialCountLatched) {
		pool.requiredInstanceCount = pool.observedMaxActive + POOL_BUFFER_INSTANCES;
		pool.initialCountLatched = true;
	}

	// Arm “became None” to hint + notify
	if (auto* gsv = dynamic_cast<GStreamerVideo*>(it->get())) {
		gsv->armOnBecameNone([monitor, listId, key](GStreamerVideo*) {
			{
				std::scoped_lock<std::mutex> lk(s_mutex);
				auto mit = pools_.find(monitor);
				if (mit == pools_.end()) return;
				auto lit = mit->second.find(listId);
				if (lit == mit->second.end()) return;
				PoolInfo& pool = lit->second;

				if (pool.index.find(key) != pool.index.end() && !pool.hinted.count(key)) {
					pool.readyHints.push_back(key);
					pool.hinted.insert(key);
				}
			} // release s_mutex
			s_cv.notify_all();
			});
	}


	const bool tryErase = pool.markedForCleanup && pool.currentActive == 0;
	lk.unlock();

	if (auto* gsv = dynamic_cast<GStreamerVideo*>(key)) {
		gsv->unload();
	}
	if (tryErase) {
		std::scoped_lock<std::mutex> relk(s_mutex);
		auto mit2 = pools_.find(monitor);
		if (mit2 != pools_.end()) {
			auto lit2 = mit2->second.find(listId);
			if (lit2 != mit2->second.end()) {
				for (auto& up : lit2->second.available)
					if (auto* gsv2 = dynamic_cast<GStreamerVideo*>(up.get()))
						gsv2->disarmOnBecameNone();
				lit2->second.available.clear();
				lit2->second.index.clear();
				lit2->second.hinted.clear();
				lit2->second.readyHints.clear();
				mit2->second.erase(lit2);
				if (mit2->second.empty()) pools_.erase(mit2);
			}
		}
	}


	s_cv.notify_all(); // wake any acquirers that were blocked
}

void VideoPool::releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId) {
	if (videos.empty() || listId == -1 || shuttingDown_) return;
	for (auto& vid : videos) {
		releaseVideo(std::move(vid), monitor, listId);
	}
}

void VideoPool::cleanup(int monitor, int listId) {
	std::scoped_lock<std::mutex> lock(s_mutex);

	auto mit = pools_.find(monitor);
	if (mit == pools_.end()) return;
	auto lit = mit->second.find(listId);
	if (lit == mit->second.end()) return;

	PoolInfo& pool = lit->second;
	pool.markedForCleanup = true;

	// If nothing is active, erase immediately; otherwise, releases will trigger the erase.
	erasePoolIfIdle_nolock(monitor, listId);

	// Wake any acquirers blocked on this pool
	s_cv.notify_all();

	LOG_DEBUG("VideoPool", "Marked for cleanup: Monitor: " + std::to_string(monitor) +
		", List ID: " + std::to_string(listId));
}

void VideoPool::shutdown() {
	LOG_INFO("VideoPool", "Starting VideoPool shutdown...");
	shuttingDown_ = true;

	{
		std::scoped_lock<std::mutex> lock(s_mutex);

		// Mark everything for cleanup and wake waiters
		for (auto& [monitor, listMap] : pools_) {
			for (auto& [listId, pool] : listMap) {
				pool.markedForCleanup = true;
			}
		}
		s_cv.notify_all();

		// Disarm callbacks for all idle instances and clear all pools
		for (auto& [monitor, listMap] : pools_) {
			for (auto& [listId, pool] : listMap) {
				for (auto& up : pool.available) {
					if (auto* gsv = dynamic_cast<GStreamerVideo*>(up.get())) {
						gsv->disarmOnBecameNone();
					}
				}
			}
		}
		pools_.clear();
	}

	// Keep shuttingDown_ = true; do NOT flip it back.
	LOG_INFO("VideoPool", "VideoPool shutdown complete.");
}

void VideoPool::erasePoolIfIdle_nolock(int monitor, int listId) {
	auto mit = pools_.find(monitor);
	if (mit == pools_.end()) return;
	auto lit = mit->second.find(listId);
	if (lit == mit->second.end()) return;

	auto& pool = lit->second;
	if (!pool.markedForCleanup || pool.currentActive != 0) return;

	// Disarm callbacks on idle instances before destruction
	for (auto& up : pool.available) {
		if (!up) continue;
		if (up->getActualState() != IVideo::VideoState::None) {
			if (auto* gsv = dynamic_cast<GStreamerVideo*>(up.get())) {
				gsv->unload(); // synchronous to avoid racing destruction
			}
		}
		if (auto* gsv = dynamic_cast<GStreamerVideo*>(up.get())) {
			gsv->disarmOnBecameNone();
		}
	}

	pool.available.clear();
	pool.index.clear();
	pool.hinted.clear();
	pool.readyHints.clear();

	mit->second.erase(lit);
	if (mit->second.empty()) pools_.erase(mit);
}

inline std::string VideoPool::poolStateStr(int monitor, int listId, const VideoPool::PoolInfo& p) {
	return "Mon:" + std::to_string(monitor) +
		" List:" + std::to_string(listId) +
		" Active=" + std::to_string(p.currentActive) +
		" Avail=" + std::to_string(p.available.size()) +
		" Hints=" + std::to_string(p.readyHints.size()) +
		" Req=" + std::to_string(p.requiredInstanceCount) +
		(p.initialCountLatched ? " LATCHED" : " PRELATCH") +
		(p.markedForCleanup ? " CLEANUP" : "");
}
