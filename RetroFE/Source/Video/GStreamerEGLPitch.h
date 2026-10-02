#pragma once
#include <gst/video/video-format.h>
#include <gst/video/video-tile.h>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// VideoMeta encodes SAND strides as tile counts, while EGL expects the
// vertical column pitch. Native DRM descriptors already contain EGL pitches.
inline int gstreamerEGLPlanePitch(const GstVideoFormatInfo* format, unsigned plane, int stride) {
    // These formats are currently supplied by Raspberry Pi's GStreamer patches.
    // Runtime names keep this compatible with unpatched headers and libraries.
    if (!format || !format->name ||
        (std::strcmp(format->name, "NV12_128C8") != 0 &&
         std::strcmp(format->name, "NV12_10LE32_128C8") != 0))
        return stride;

    if (!GST_VIDEO_FORMAT_INFO_IS_TILED(format) ||
        plane >= format->n_planes || plane >= GST_VIDEO_MAX_PLANES || stride <= 0)
        throw std::runtime_error("invalid SAND plane metadata");

    const auto columns = GST_VIDEO_TILE_X_TILES(stride);
    const auto rows = GST_VIDEO_TILE_Y_TILES(stride);
    const auto tileHeight = GST_VIDEO_FORMAT_INFO_TILE_HEIGHT(format, plane);
    const uint64_t pitch = uint64_t(rows) * tileHeight;
    if (!columns || !rows || !tileHeight || pitch > INT_MAX)
        throw std::runtime_error("invalid SAND column pitch");
    return static_cast<int>(pitch);
}
