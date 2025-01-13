// VideoPool.h
#pragma once
#include <unordered_map>
#include <memory>
#include "../Video/IVideo.h"  // or wherever it is
#include "../Video/GStreamerVideo.h"

class VideoPool {
public:
    static GStreamerVideo* acquireVideo(int monitor, bool softOverlay);
    static void releaseVideo(GStreamerVideo* vid, int monitor);

    void shutdown();

private:
    static std::unordered_map<int, std::vector<GStreamerVideo*>> pools_;
    // Possibly a mutex if multi-threaded
};