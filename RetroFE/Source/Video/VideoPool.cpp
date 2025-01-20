#include <unordered_map>
#include <vector>
#include "VideoPool.h"

// Define static members for multiple pools
std::unordered_map<int, std::unordered_map<int, std::vector<GStreamerVideo*>>> VideoPool::pools_;
std::unordered_map<int, std::unordered_map<int, int>> VideoPool::maxInstances_;
std::unordered_map<int, std::unordered_map<int, bool>> VideoPool::extraInstanceCreated_;

GStreamerVideo* VideoPool::acquireVideo(int monitor, int listId, bool softOverlay) {
    if (listId == -1) {
        return new GStreamerVideo(monitor);
    }

    auto& pool = pools_[monitor][listId];
    auto& extraCreated = extraInstanceCreated_[monitor][listId];

    // Ensure each pool gets one extra instance
    if (!extraCreated && !pool.empty()) {
        extraCreated = true;
        maxInstances_[monitor][listId]++;
        LOG_DEBUG("VideoPool", "First pool use detected for Monitor: " + std::to_string(monitor) +
            ", List ID: " + std::to_string(listId) + ", creating an extra instance.");
        return new GStreamerVideo(monitor);
    }

    if (!pool.empty()) {
        GStreamerVideo* vid = pool.back();
        pool.pop_back();
        vid->setSoftOverlay(softOverlay);
        LOG_DEBUG("VideoPool", "Reusing a GStreamerVideo from the pool. Monitor: " +
            std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
            ", Remaining instances: " + std::to_string(pool.size()));
        return vid;
    }

    // If no video available in pool, create a new one
    maxInstances_[monitor][listId]++;
    LOG_DEBUG("VideoPool", "No available video in pool. Creating new instance for Monitor: " +
        std::to_string(monitor) + ", List ID: " + std::to_string(listId));
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid, int monitor, int listId) {
    if (!vid) return;

    if (listId == -1) {
        vid->stop();
        delete vid;
        return;
    }

    vid->unload();
    auto& pool = pools_[monitor][listId];

    pool.push_back(vid);

    LOG_DEBUG("VideoPool", "Video instance released to pool. Monitor: " +
        std::to_string(monitor) + ", List ID: " + std::to_string(listId) +
        ", Current pool size: " + std::to_string(pool.size()) +
        ", Max required instances: " + std::to_string(maxInstances_[monitor][listId]));
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
    maxInstances_.clear();
    extraInstanceCreated_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete, all pooled videos destroyed.");
}
