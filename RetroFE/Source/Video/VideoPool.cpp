// VideoPool.cpp

#include "VideoPool.h"
#include "../Utility/Log.h"          // or wherever your LOG_* macros reside

// Define the static member 'pools_'
std::unordered_map<int, std::unordered_map<int, std::vector<GStreamerVideo*>>> VideoPool::pools_;

GStreamerVideo* VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    // If listId is -1, do not attempt to acquire from the pool
    if (listId == -1) {
        LOG_DEBUG("VideoPool", "Creating a new GStreamerVideo instance for monitor: " + std::to_string(monitor) + " without pooling (listId is -1)");
        return new GStreamerVideo(monitor);
    }

    auto& pool = pools_[monitor][listId];

    if (!pool.empty()) {
        GStreamerVideo* vid = pool.back();
        pool.pop_back();
        LOG_DEBUG("VideoPool", "Reusing a GStreamerVideo from the pool for monitor: " + std::to_string(monitor) + " ScrollingList Id: " + std::to_string(listId));
        if (!vid->initialize()) {
            LOG_ERROR("VideoPool", "Failed to re-initialize GStreamerVideo from pool. Creating new instance.");
            delete vid;
            return new GStreamerVideo(monitor);
        }
        vid->setSoftOverlay(softOverlay);
        return vid;
    }

    LOG_DEBUG("VideoPool", "No idle GStreamerVideo in the pool for monitor: " + std::to_string(monitor) + " ScrollingList Id: " + std::to_string(listId) + "; creating a new one");
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    // If listId is -1, do not add to the pool; stop and delete the instance instead
    if (listId == -1) {
        LOG_DEBUG("VideoPool", "Stopping and deleting GStreamerVideo instance for monitor: " + std::to_string(monitor) + " without pooling (listId is -1)");
        vid->stop();
        delete vid;
        return;
    }

    vid->unload();
    pools_[monitor][listId].push_back(vid);
    LOG_DEBUG("VideoPool", "GStreamerVideo returned to the pool for monitor: " + std::to_string(monitor) + " ScrollingList Id: " + std::to_string(listId) + ", current pool size: " + std::to_string(pools_[monitor][listId].size()));
}

void VideoPool::shutdown() {
    for (auto& [monitor, listPools] : pools_) {
        for (auto& [listId, pool] : listPools) {
            for (auto* vid : pool) {
                vid->stop();
                delete vid;
            }
            pool.clear();
        }
        listPools.clear();
    }
    pools_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete, all pooled videos destroyed.");
}