#pragma once

#include <SDL3/SDL.h>
#include <gst/video/video.h>

namespace GStreamerCpuFrame {
inline const char* caps() { return "video/x-raw,format=(string){I420,NV12}"; }

inline SDL_PixelFormat format(const GstVideoInfo& info) {
    switch (GST_VIDEO_INFO_FORMAT(&info)) {
    case GST_VIDEO_FORMAT_I420: return SDL_PIXELFORMAT_IYUV;
    case GST_VIDEO_FORMAT_NV12: return SDL_PIXELFORMAT_NV12;
    case GST_VIDEO_FORMAT_RGBA: return SDL_PIXELFORMAT_ABGR8888;
    default: return SDL_PIXELFORMAT_UNKNOWN;
    }
}

inline SDL_Colorspace colorspace(const GstVideoInfo& info) {
    if (GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_RGBA) return SDL_COLORSPACE_SRGB;
    const bool full = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    switch (info.colorimetry.matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT601:
        return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    case GST_VIDEO_COLOR_MATRIX_BT709:
        return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
        return full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
    default:
        return GST_VIDEO_INFO_HEIGHT(&info) > 576
            ? (full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED)
            : (full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED);
    }
}

inline bool isSystemMemory(GstBuffer* buffer) {
    if (!buffer || !gst_buffer_n_memory(buffer)) return false;
    for (guint i = 0; i < gst_buffer_n_memory(buffer); ++i)
        if (!gst_memory_is_type(gst_buffer_peek_memory(buffer, i), GST_ALLOCATOR_SYSMEM)) return false;
    return true;
}
}
