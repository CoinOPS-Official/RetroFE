// VideoPool.cpp

#include "VideoPool.h"
#include "../Utility/Log.h"          // or wherever your LOG_* macros reside
#include "../Database/Configuration.h" // if needed, depends on your usage

// Define the static member 'pool_'
std::unordered_map<int, std::vector<GStreamerVideo*>> VideoPool::pools_;

GStreamerVideo* VideoPool::acquireVideo(int monitor, bool softOverlay)
{
    // Optional: lock a mutex here if multi-threaded

    // 1. Access the pool for the specified monitor
    auto& pool = pools_[monitor]; // This creates an empty vector if no pool exists for the monitor

    // 2. If there's an idle instance in the monitor's pool, return it.
    if (!pool.empty())
    {
        GStreamerVideo* vid = pool.back();
        pool.pop_back();
        LOG_DEBUG("VideoPool", "Reusing a GStreamerVideo from the pool for monitor " + std::to_string(monitor));

        // Reinitialize the video instance if needed
        if (!vid->initialize()) {
            LOG_ERROR("VideoPool", "Failed to re-initialize GStreamerVideo from pool. Creating new instance.");
            delete vid;
            return new GStreamerVideo(monitor);
        }

        // Update the softOverlay flag
        vid->setSoftOverlay(softOverlay);

        return vid;
    }

    // 3. If no idle instance is available, create a new one for this monitor.
    LOG_DEBUG("VideoPool", "No idle GStreamerVideo in the pool for monitor " + std::to_string(monitor) + "; creating a new one");
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid, int monitor)
{
    if (!vid) return;

    // Optional: lock a mutex here if multi-threaded

    // 1. Move the video instance to an "unloaded" state
    vid->unload();

    // 2. Add it back to the pool for the corresponding monitor
    pools_[monitor].push_back(vid);

    LOG_DEBUG("VideoPool", "GStreamerVideo returned to the pool for monitor " + std::to_string(monitor) +
        ", current pool size = " + std::to_string(pools_[monitor].size()));
}


void VideoPool::shutdown()
{
    // Optional: lock a mutex here if multi-threaded

    // Destroy all video instances in all monitor-specific pools
    for (auto& [monitor, pool] : pools_)
    {
        for (auto* vid : pool)
        {
            vid->stop();    // Fully stop and release resources
            delete vid;
        }
        pool.clear();
    }
    pools_.clear();

    LOG_DEBUG("VideoPool", "VideoPool shutdown complete, all pooled videos destroyed.");
}