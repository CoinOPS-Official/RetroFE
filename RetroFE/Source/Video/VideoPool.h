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

#pragma once

#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <condition_variable>
#include "GStreamerVideo.h"

class VideoPool {
public:
	using VideoPtr = std::unique_ptr<IVideo>;  // <-- Use base type

	static VideoPtr acquireVideo(int monitor, int listId, bool softOverlay);
	static void releaseVideo(VideoPtr vid, int monitor, int listId);
	static void releaseVideoBatch(std::vector<VideoPtr> videos, int monitor, int listId);
	static void cleanup(int monitor, int listId);
	static void shutdown();
	static std::atomic<bool> shuttingDown_;

private:

    struct PoolInfo {
        // Idle instances, oldest-first (we insert at back)
        std::list<VideoPtr> available;

        // Fast path: raw pointers to items known to be VideoState::None
        std::deque<IVideo*> readyHints;

        // O(1) lookup/validation & removal by pointer
        std::unordered_map<IVideo*, std::list<VideoPtr>::iterator> index;

        // Prevent duplicate hints spamming the queue
        std::unordered_set<IVideo*> hinted;

        // Bookkeeping
        size_t currentActive = 0;
        size_t observedMaxActive = 0;
        size_t requiredInstanceCount = 0;
        bool initialCountLatched = false;

        // Sync
        std::mutex poolMutex;
        std::condition_variable poolCond;
        bool markedForCleanup = false;
        bool cleanupInProgress = false;
    };

	using ListPoolMap = std::unordered_map<int, PoolInfo>;
	using PoolMap = std::unordered_map<int, ListPoolMap>;

	static PoolMap pools_;
	static std::mutex s_poolsMutex;

	static PoolInfo& getPoolInfo(int monitor, int listId);
	static bool checkPoolHealth(int monitor, int listId);
	static void cleanup_nolock(int monitor, int listId);

};