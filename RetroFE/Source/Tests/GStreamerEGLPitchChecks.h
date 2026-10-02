#pragma once
#include "../Video/GStreamerEGLPitch.h"
#include <initializer_list>

template<class Require>
void gstreamerEGLPitchChecks(Require require) {
    const auto* nv12 = gst_video_format_get_info(GST_VIDEO_FORMAT_NV12);
    const auto* p010 = gst_video_format_get_info(GST_VIDEO_FORMAT_P010_10LE);
    const auto* otherTiled = gst_video_format_get_info(GST_VIDEO_FORMAT_NV12_8L128);
    const int tileStride = GST_VIDEO_TILE_MAKE_STRIDE(15, 136);
    require(gstreamerEGLPlanePitch(nv12, 0, 2048) == 2048 &&
            gstreamerEGLPlanePitch(p010, 1, 4096) == 4096,
            "Linear NV12 and P010 pitches remain unchanged");
    require(gstreamerEGLPlanePitch(otherTiled, 0, tileStride) == tileStride &&
            gstreamerEGLPlanePitch(nullptr, 0, 2048) == 2048,
            "Non-SAND tiled and unknown format pitches remain unchanged");

    // Fixtures exercise the patched format contract even with stock GStreamer.
    GstVideoFormatInfo sand = *nv12;
    sand.flags = static_cast<GstVideoFormatFlags>(sand.flags | GST_VIDEO_FORMAT_FLAG_TILED);
    sand.tile_info[0].height = 8;
    sand.tile_info[1].height = 4;
    for (const char* name : {"NV12_128C8", "NV12_10LE32_128C8"}) {
        sand.name = name;
        require(gstreamerEGLPlanePitch(&sand, 0, tileStride) == 1088 &&
                gstreamerEGLPlanePitch(&sand, 1, tileStride) == 544,
                "SAND luma and chroma use vertical tile pitch including padding");
        require(gstreamerEGLPlanePitch(&sand, 0, GST_VIDEO_TILE_MAKE_STRIDE(20, 136)) == 1088,
                "SAND pitch depends on column height rather than horizontal tile count");
    }
    auto rejected = [&](unsigned plane, int stride) {
        try { gstreamerEGLPlanePitch(&sand, plane, stride); }
        catch (const std::exception&) { return true; }
        return false;
    };
    require(rejected(2, tileStride) && rejected(0, 0) && rejected(0, -1) &&
            rejected(0, GST_VIDEO_TILE_MAKE_STRIDE(0, 136)) &&
            rejected(0, GST_VIDEO_TILE_MAKE_STRIDE(15, 0)),
            "Invalid SAND plane indexes and tile grids are rejected");
    sand.tile_info[0].height = 0;
    require(rejected(0, tileStride), "Missing SAND tile geometry is rejected");
    sand.tile_info[0].height = INT_MAX;
    require(rejected(0, tileStride), "SAND pitch overflow is rejected");
}
