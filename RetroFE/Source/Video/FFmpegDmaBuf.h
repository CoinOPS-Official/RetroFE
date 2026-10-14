#pragma once
#include "EGLVideoInterop.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
}
#include <climits>
#include <stdexcept>

// Pure metadata adapter; never maps pixels or takes ownership of borrowed FDs.
inline EGLDmaBufFrame ffmpegDmaBufFrame(const std::shared_ptr<AVFrame>& source, const std::shared_ptr<AVFrame>& mapped) {
    auto require = [](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); };
    auto fourcc = [](char a,char b,char c,char d) { return uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24); };
    require(source->hw_frames_ctx && mapped->data[0], "missing exported DMA-BUF descriptor");
    const auto& hw = *reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data);
    const auto& drm = *reinterpret_cast<AVDRMFrameDescriptor*>(mapped->data[0]);
    require(drm.nb_objects > 0 && drm.nb_objects <= 4 && drm.nb_layers > 0 && drm.nb_layers <= 2, "unsupported DMA-BUF objects/layers");
    const bool native = source->format == AV_PIX_FMT_DRM_PRIME;
    const auto nv12Fourcc = fourcc('N','V','1','2');
    const auto p010Fourcc = fourcc('P','0','1','0');
    const auto p030Fourcc = fourcc('P','0','3','0');
    // Native Pi frames use FFmpeg-specific RPI4 software format enums. Their
    // composed DRM layer describes the actual storage without enum extensions.
    if (native)
        require(drm.nb_layers == 1 && (drm.layers[0].format == nv12Fourcc ||
            drm.layers[0].format == p010Fourcc || drm.layers[0].format == p030Fourcc),
            "unsupported native DRM storage format");
    else
        require(hw.sw_format == AV_PIX_FMT_NV12 || hw.sw_format == AV_PIX_FMT_P010LE, "EGL import supports NV12/P010 SDR");
    EGLDmaBufFrame out;
    out.owner = mapped;
    out.width = hw.width; out.height = hw.height;
    require(source->width > 0 && source->height > 0 && source->crop_left < size_t(source->width) &&
        source->crop_right < size_t(source->width)-source->crop_left && source->crop_top < size_t(source->height) &&
        source->crop_bottom < size_t(source->height)-source->crop_top, "invalid decoded crop");
    out.crop = {int(source->crop_left),int(source->crop_top),
        source->width-int(source->crop_left+source->crop_right),source->height-int(source->crop_top+source->crop_bottom)};
    out.fourcc = native ? drm.layers[0].format : (hw.sw_format == AV_PIX_FMT_NV12 ? nv12Fourcc : p010Fourcc);
    const bool nv12 = out.fourcc == nv12Fourcc;
    // VAAPI exports either a composed layer or separate Y/UV layers. Only
    // combine the recognized R/GR pair; never guess an arbitrary layer layout.
    for (int l=0;l<drm.nb_layers;++l) {
        const auto& layer=drm.layers[l];
        const auto expected = drm.nb_layers==1 ? out.fourcc :
            (l==0 ? (nv12?fourcc('R','8',' ',' '):fourcc('R','1','6',' ')) :
                     (nv12?fourcc('G','R','8','8'):fourcc('G','R','3','2')));
        require(layer.format==expected && layer.nb_planes==(drm.nb_layers==1?2:1), "unsupported DMA-BUF layer layout");
        for(int i=0;i<layer.nb_planes;++i) {
            require(out.planeCount < int(out.planes.size()), "too many DMA-BUF planes");
            const auto& plane=layer.planes[i];
            require(plane.object_index>=0 && plane.object_index<drm.nb_objects, "invalid DMA-BUF object index");
            const auto& object=drm.objects[plane.object_index];
            require(object.fd>=0 && plane.offset>=0 && plane.offset<=INT_MAX && size_t(plane.offset)<object.size &&
                plane.pitch>0 && plane.pitch<=INT_MAX && object.format_modifier!=((uint64_t(1)<<56)-1), "invalid DMA-BUF plane/modifier");
            out.planes[out.planeCount++]={object.fd,int(plane.offset),int(plane.pitch),object.format_modifier};
        }
    }
    require(source->color_trc!=AVCOL_TRC_SMPTE2084 && source->color_trc!=AVCOL_TRC_ARIB_STD_B67, "HDR requires tone mapping");
    require(source->colorspace==AVCOL_SPC_UNSPECIFIED || source->colorspace==AVCOL_SPC_BT709 ||
        source->colorspace==AVCOL_SPC_BT470BG || source->colorspace==AVCOL_SPC_SMPTE170M, "unsupported YUV matrix");
    out.bt709 = source->colorspace==AVCOL_SPC_BT709 || (source->colorspace==AVCOL_SPC_UNSPECIFIED && source->height>576);
    out.fullRange = source->color_range==AVCOL_RANGE_JPEG;
    if(source->chroma_location==AVCHROMA_LOC_LEFT) { out.chromaX=0; out.chromaY=1; }
    else if(source->chroma_location==AVCHROMA_LOC_CENTER) { out.chromaX=1; out.chromaY=1; }
    else if(source->chroma_location==AVCHROMA_LOC_TOPLEFT) { out.chromaX=0; out.chromaY=0; }
    else require(source->chroma_location==AVCHROMA_LOC_UNSPECIFIED, "unsupported chroma siting");
    return out;
}
