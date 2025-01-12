// VideoPool.cpp

#include "VideoPool.h"
#include "../Utility/Log.h"          // or wherever your LOG_* macros reside
#include "../Database/Configuration.h" // if needed, depends on your usage

// Define the static member 'pool_'
std::vector<GStreamerVideo*> VideoPool::pool_;

GStreamerVideo* VideoPool::acquireVideo(int monitor, bool softOverlay)
{
    // Optional: lock a mutex here if multi-threaded

    // 1. If there's an idle instance in 'pool_', return it.
    if (!pool_.empty())
    {
        GStreamerVideo* vid = pool_.back();
        pool_.pop_back();
        LOG_DEBUG("VideoPool", "Reusing a GStreamerVideo from the pool");

        // Because we may have previously called 'unload()' or changed states,
        // re-initialize if needed.
        if (!vid->initialize()) {
            // If initialization fails for some reason, destroy it and create a fresh one
            LOG_ERROR("VideoPool", "Failed to re-initialize GStreamerVideo from pool. Creating new instance.");
            delete vid;
            return new GStreamerVideo(monitor);
        }

        // Update the monitor, softOverlay, etc. if needed
        vid->setSoftOverlay(softOverlay);

        return vid;
    }

    // 2. If no idle instance is available, create a brand new one.
    LOG_DEBUG("VideoPool", "No idle GStreamerVideo in the pool; creating a new one");
    return new GStreamerVideo(monitor);
}

void VideoPool::releaseVideo(GStreamerVideo* vid)
{
    if (!vid) return;

    // Optional: lock a mutex here if multi-threaded

    // Move pipeline to a non-playing state but do NOT unref or set pipeline to NULL
    // We'll rely on the 'unload()' method in GStreamerVideo to handle that logic:
    vid->unload();  

    // Now that it’s “unloaded,” push it back into the pool for reuse
    pool_.push_back(vid);

    LOG_DEBUG("VideoPool", "GStreamerVideo returned to the pool, current size = " + std::to_string(pool_.size()));
}

void VideoPool::shutdown()
{
    // If you want a clean shutdown, do it here
    for (auto* vid : pool_)
    {
        // Move pipeline fully to NULL, unref, free memory, etc.
        vid->stop();    // or a specialized teardown that sets GST_STATE_NULL
        delete vid;
    }
    pool_.clear();
    LOG_DEBUG("VideoPool", "VideoPool shutdown complete, all pooled videos destroyed.");
}