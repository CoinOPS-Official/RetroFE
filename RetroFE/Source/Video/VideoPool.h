// VideoPool.h
#pragma once
#include <unordered_map>
#include <memory>
#include "../Video/IVideo.h"  // or wherever it is
#include "../Video/GStreamerVideo.h"

class VideoPool {
public:
    static GStreamerVideo* acquireVideo(int monitor, int listId, bool softOverlay);
    static void releaseVideo(GStreamerVideo* vid, int monitor, int listId);
    static void shutdown();

private:
    static std::unordered_map<int, std::unordered_map<int, std::vector<GStreamerVideo*>>> pools_;
};