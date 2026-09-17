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

#include <list>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <deque>
#include <unordered_set>

class IVideo;

// Main-thread-only pool for IVideo instances.
// listId == -1 => non-pooled ("one-shot") videos.
class VideoPool {
public:
    static constexpr size_t POOL_BUFFER_INSTANCES = 2;

    // C++20: Switched to shared_ptr to support atomic callback safety 
    // and enable_shared_from_this in the GStreamer implementation.
    using VideoPtr = std::shared_ptr<IVideo>;

    static VideoPtr acquireVideo(int monitor, int listId, bool softOverlay);

    // Reuse an already-warm READY instance for a playlist handoff.
    // Never creates a new instance.
    static VideoPtr acquireWarmForReset(int monitor, int listId, bool softOverlay);

    static void releaseVideo(VideoPtr vid, int monitor, int listId);
    static void releaseVideoBatch(std::vector<VideoPtr>& videos, int monitor, int listId);

    // Marks a pool for cleanup. If there are no active instances, the pool is erased immediately.
    static void cleanup(int monitor, int listId);

    static void decrementActive(int monitor, int listId);

    // Clears all pools and disables further pooling.
    static void shutdown();

    // Hint the pool that we'd like at least desiredTotal instances available+active.
    static void reserveCapacity(int monitor, int listId, size_t desiredTotal);

    static bool isShuttingDown() { return shuttingDown_; }
    static bool shuttingDown_;

    static void reset(int monitor, int listId);

private:
    struct PoolInfo {
        std::deque<VideoPtr> ready;
        std::deque<VideoPtr> draining;

        size_t currentActive = 0;
        size_t requiredInstanceCount = 0;
        size_t observedMaxActive = 0;

        // Exact identity tracking avoids release-order bugs when an old
        // playlist handoff overlaps newly-created videos.
        std::unordered_set<IVideo*> activeVideos;
        std::unordered_set<IVideo*> retireOnRelease;

        bool initialCountLatched = false;
        bool markedForCleanup = false;
    };

    using ListPoolMap = std::unordered_map<int, PoolInfo>;
    using PoolMap = std::unordered_map<int, ListPoolMap>;

    static PoolMap pools_;
    static void pumpDrainingToReady(PoolInfo& pool);
    static void erasePoolIfIdle(int monitor, int listId);
    static std::string poolStateStr(int monitor, int listId, const PoolInfo& p);

    // Policy: create a new shared_ptr instance via std::make_shared
    static VideoPtr createNewVideo(int monitor, bool softOverlay);
};