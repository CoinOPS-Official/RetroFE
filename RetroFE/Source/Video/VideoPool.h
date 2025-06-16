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
#include <string>
#include <mutex>
#include "GStreamerVideo.h"

class VideoPool {
public:
    using VideoPtr = std::unique_ptr<IVideo>;  // <-- Use base type

    static VideoPtr acquireVideo(int monitor, int listId, bool softOverlay);
    static void releaseVideo(VideoPtr vid, int monitor, int listId);
    static void cleanup(int monitor, int listId);
    static void shutdown();

private:
   
    struct PoolInfo {
        std::deque<VideoPtr> instances;    // Store unique_ptr<IVideo>
        size_t currentActive = 0;
        size_t observedMaxActive = 0;
        size_t requiredInstanceCount = 0;
        bool initialCountLatched = false;
        std::mutex poolMutex;
        bool markedForCleanup = false;
    };

    using ListPoolMap = std::unordered_map<int, PoolInfo>;
    using PoolMap = std::unordered_map<int, ListPoolMap>;

    static PoolMap pools_;

    static PoolInfo& getPoolInfo(int monitor, int listId);
    static bool checkPoolHealth(int monitor, int listId);
};