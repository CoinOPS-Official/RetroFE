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
#include "../Video/IVideo.h"
#include "../Video/GStreamerVideo.h"

class VideoPool {
public:
    static GStreamerVideo* acquireVideo(int monitor, int listId, bool softOverlay);
    static void releaseVideo(GStreamerVideo* vid, int monitor, int listId);
    static void cleanup(int monitor, int listId);
    static void shutdown();

private:
    struct PoolInfo {
        std::vector<GStreamerVideo*> instances;  // Pool of available video instances
        size_t currentActive = 0;                // Number of currently active instances
        bool poolInitialized = false;            // Whether pool has had its first release
        bool hasExtraInstance = false;           // Whether the extra instance has been created
        size_t maxRequired = 0;                  // Maximum number of instances needed
    };

    // Monitor -> ListId -> Pool mapping
    using PoolMap = std::unordered_map<int, std::unordered_map<int, PoolInfo>>;
    static PoolMap pools_;
};