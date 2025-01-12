// VideoPool.h
#pragma once
#include <vector>
#include <memory>
#include "../Video/IVideo.h"  // or wherever it is
#include "../Video/GStreamerVideo.h"

class VideoPool {
public:
    static GStreamerVideo* acquireVideo(int monitor, bool softOverlay);
    static void releaseVideo(GStreamerVideo* vid);

    void shutdown();

private:
    static std::vector<GStreamerVideo*> pool_;
    // Possibly a mutex if multi-threaded
};