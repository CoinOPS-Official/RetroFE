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

#include <unordered_map>
#include <vector>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <condition_variable>
#include "../Video/IVideo.h"
#include "../Video/GStreamerVideo.h"

class VideoPool {
public:
    static std::unique_ptr<IVideo> acquireVideo(int monitor, int listId, bool softOverlay);
    static void releaseVideo(std::unique_ptr<GStreamerVideo> vid, int monitor, int listId);
    static void cleanup(int monitor, int listId);
    static void shutdown();
    // Health check method
    static bool checkPoolHealth(int monitor, int listId);

private:
    struct PoolInfo {
        std::deque<std::unique_ptr<GStreamerVideo>> instances;
        std::atomic<size_t> currentActive{0};
        std::timed_mutex poolMutex;
        std::condition_variable_any waitCondition;  // Add this line
        std::atomic<size_t> observedMaxActive{ 0 };  // Track observed maximum active instances
        std::atomic<bool> initialCountLatched{false};
        std::atomic<size_t> requiredInstanceCount{0}; // The latched target count (excluding the +1 buffer)

        PoolInfo() = default;
        PoolInfo(const PoolInfo&) = delete;
        PoolInfo& operator=(const PoolInfo&) = delete;
        PoolInfo(PoolInfo&&) = delete;
        PoolInfo& operator=(PoolInfo&&) = delete;
    };
    using PoolInfoPtr = std::shared_ptr<PoolInfo>;
    using ListPoolMap = std::map<int, PoolInfoPtr>;
    using PoolMap = std::map<int, ListPoolMap>;
    static PoolMap pools_;
    static std::shared_mutex mapMutex_;

    static PoolInfoPtr getPoolInfo(int monitor, int listId);
};
