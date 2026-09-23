/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "FFmpegVideo.h"
#include "../Database/Configuration.h"
#include "../SDL.h"
#include "../Sound/AudioBus.h"
#include "../Utility/Log.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#ifdef RETROFE_HAVE_D3D12
#include "D3D12VideoInterop.h"
extern "C" {
#include <libavutil/hwcontext_d3d12va.h>
}
#endif
#ifdef _WIN32
#include <d3d11.h>
#include "D3D11VideoInterop.h"
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif
#ifdef RETROFE_HAVE_EGL_DMABUF
#include "FFmpegDmaBuf.h"
extern "C" {
#include <libavutil/hwcontext_drm.h>
}
#include <climits>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>

namespace {
struct Point2D {
    double x, y;
};
static inline std::array<double, 9> computePerspectiveMatrixFromCorners(int width, int height,
                                                                        const std::array<Point2D, 4> &pts) {
    constexpr double EPSILON = 1e-9;

    const Point2D A = pts[0];
    const Point2D B = pts[1];
    const Point2D D = pts[2];
    const Point2D C = pts[3];

    double M11 = B.x - C.x;
    double M12 = D.x - C.x;
    double M21 = B.y - C.y;
    double M22 = D.y - C.y;
    double RHS1 = A.x - C.x;
    double RHS2 = A.y - C.y;

    double denom = M11 * M22 - M12 * M21;
    if (std::abs(denom) < EPSILON)
        return {1, 0, 0, 0, 1, 0, 0, 0, 1};

    double X = (RHS1 * M22 - RHS2 * M12) / denom;
    double Y = (M11 * RHS2 - M21 * RHS1) / denom;

    double g = X - 1.0;
    double h = Y - 1.0;

    double a = X * B.x - A.x;
    double d = X * B.y - A.y;
    double b = Y * D.x - A.x;
    double e = Y * D.y - A.y;
    double c = A.x;
    double f = A.y;

    std::array<double, 9> Hf = {a, b, c, d, e, f, g, h, 1.0};

    double det = Hf[0] * (Hf[4] * Hf[8] - Hf[5] * Hf[7]) - Hf[1] * (Hf[3] * Hf[8] - Hf[5] * Hf[6]) +
                 Hf[2] * (Hf[3] * Hf[7] - Hf[4] * Hf[6]);
    if (std::abs(det) < EPSILON)
        return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double invDet = 1.0 / det;
    std::array<double, 9> H = {
        (Hf[4] * Hf[8] - Hf[5] * Hf[7]) * invDet, (Hf[2] * Hf[7] - Hf[1] * Hf[8]) * invDet,
        (Hf[1] * Hf[5] - Hf[2] * Hf[4]) * invDet, (Hf[5] * Hf[6] - Hf[3] * Hf[8]) * invDet,
        (Hf[0] * Hf[8] - Hf[2] * Hf[6]) * invDet, (Hf[2] * Hf[3] - Hf[0] * Hf[5]) * invDet,
        (Hf[3] * Hf[7] - Hf[4] * Hf[6]) * invDet, (Hf[1] * Hf[6] - Hf[0] * Hf[7]) * invDet,
        (Hf[0] * Hf[4] - Hf[1] * Hf[3]) * invDet};

    for (double &val : H) {
        val /= H[8];
    }

    H[0] *= width;
    H[1] *= width;
    H[2] *= width;
    H[3] *= height;
    H[4] *= height;
    H[5] *= height;

    return H;
}
// FFmpeg's default callback writes to stderr, which is not reliably captured
// by the GUI logger. Install once before creating any FFmpeg device or worker.
bool ffmpegDiagnostics() {
    static const bool enabled = [] { const char* p = std::getenv("RETROFE_FFMPEG_DIAGNOSTICS"); return p && std::strcmp(p,"1")==0; }();
    return enabled;
}
thread_local std::string ffmpegMedia;
void ffmpegLog(void* context, int level, const char* format, va_list args) {
    if (level > (ffmpegDiagnostics() ? AV_LOG_INFO : AV_LOG_WARNING)) return;
    try {
        char text[4096]{};
        int prefix=1;
        av_log_format_line2(context,level,format,args,text,sizeof(text),&prefix);
        std::string message=text;
        while(!message.empty() && (message.back()=='\n' || message.back()=='\r')) message.pop_back();
        if(message.empty()) return;
        if(!ffmpegMedia.empty()) message=ffmpegMedia+": "+message;
        auto zone=level<=AV_LOG_ERROR?Logger::ZONE_ERROR:level<=AV_LOG_WARNING?Logger::ZONE_WARNING:Logger::ZONE_INFO;
        if(ffmpegDiagnostics()) Logger::write(zone,"FFmpeg",message);
        else if(level<=AV_LOG_ERROR) { LOG_ERROR("FFmpeg",message); }
        else { LOG_WARNING("FFmpeg",message); }
    } catch (...) { /* Never unwind through a C logging callback. */ }
}
void installFFmpegLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_callback(ffmpegLog);
        if(ffmpegDiagnostics()) Logger::write(Logger::ZONE_INFO,"FFmpegVideo",
            std::string("Diagnostics enabled; FFmpeg ")+av_version_info());
    });
}
using Frame = std::shared_ptr<AVFrame>;
Frame frame() {
    return Frame(av_frame_alloc(), [](AVFrame *p) { av_frame_free(&p); });
}
void check(int result, const char *where) {
    if (result < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, text, sizeof(text));
        throw std::runtime_error(std::string(where) + ": " + text);
    }
}

constexpr AVRational ns{1, 1000000000};
const SDL_BlendMode softBlend = SDL_ComposeCustomBlendMode(
    SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
    SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
struct Video {
    Frame data;
    int64_t pts;
    bool cpuPrepared = false;
    bool perspective = false;
#ifdef RETROFE_HAVE_EGL_DMABUF
    Frame drm;
#endif
};
struct Audio {
    std::vector<float> data;
    int64_t pts;
};

struct SharedHwEntry {
    AVBufferRef* deviceCtx = nullptr;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    int refCount = 0;
};
static std::mutex s_sharedHwMutex;
static std::map<SDL_Renderer*, SharedHwEntry> s_sharedHwEntries;

static SharedHwEntry acquireSharedHardware(SDL_Renderer* renderer) {
    if (!renderer) return {};
    std::lock_guard<std::mutex> lock(s_sharedHwMutex);
    auto it = s_sharedHwEntries.find(renderer);
    if (it != s_sharedHwEntries.end() && it->second.deviceCtx) {
        ++it->second.refCount;
        return { av_buffer_ref(it->second.deviceCtx), it->second.format, it->second.refCount };
    }

    AVBufferRef* hw = nullptr;
    AVPixelFormat fmt = AV_PIX_FMT_NONE;

#ifdef RETROFE_HAVE_D3D12
    if (Configuration::HardwareVideoAccel) {
        auto* device = static_cast<ID3D12Device*>(
            SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                                   SDL_PROP_RENDERER_D3D12_DEVICE_POINTER, nullptr));
        if (device) {
            hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
            if (hw) {
                auto* d = static_cast<AVD3D12VADeviceContext*>(
                    reinterpret_cast<AVHWDeviceContext*>(hw->data)->hwctx);
                d->device = device;
                d->resource_flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
                device->AddRef();
                if (av_hwdevice_ctx_init(hw) < 0) {
                    av_buffer_unref(&hw);
                } else {
                    fmt = AV_PIX_FMT_D3D12;
                }
            }
        }
    }
#endif
#ifdef _WIN32
    if (Configuration::HardwareVideoAccel && !hw) {
        auto* d3d11Device = static_cast<ID3D11Device*>(
            SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                                   SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
        if (d3d11Device) {
            hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (hw) {
                auto* d = static_cast<AVD3D11VADeviceContext*>(
                    reinterpret_cast<AVHWDeviceContext*>(hw->data)->hwctx);
                d->device = d3d11Device;
                d3d11Device->AddRef();
                if (av_hwdevice_ctx_init(hw) < 0) {
                    av_buffer_unref(&hw);
                } else {
                    fmt = AV_PIX_FMT_D3D11;
                }
            }
        }
    }
#endif
    if (Configuration::HardwareVideoAccel && !hw) {
#ifdef _WIN32
        const auto type = AV_HWDEVICE_TYPE_D3D11VA;
        const auto defFmt = AV_PIX_FMT_D3D11;
#elif defined(__linux__)
        const auto type = AV_HWDEVICE_TYPE_VAAPI;
        const auto defFmt = AV_PIX_FMT_VAAPI;
#elif defined(__APPLE__)
        const auto type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        const auto defFmt = AV_PIX_FMT_VIDEOTOOLBOX;
#else
        const auto type = AV_HWDEVICE_TYPE_NONE;
        const auto defFmt = AV_PIX_FMT_NONE;
#endif
        if (type != AV_HWDEVICE_TYPE_NONE &&
            av_hwdevice_ctx_create(&hw, type, nullptr, nullptr, 0) >= 0) {
            fmt = defFmt;
        }
    }

    if (hw) {
        s_sharedHwEntries[renderer] = { av_buffer_ref(hw), fmt, 1 };
    }
    return { hw, fmt, 1 };
}

static void releaseSharedHardware(SDL_Renderer* renderer) {
    if (!renderer) return;
    std::lock_guard<std::mutex> lock(s_sharedHwMutex);
    auto it = s_sharedHwEntries.find(renderer);
    if (it != s_sharedHwEntries.end()) {
        if (--it->second.refCount <= 0) {
            if (it->second.deviceCtx) {
                av_buffer_unref(&it->second.deviceCtx);
            }
            s_sharedHwEntries.erase(it);
        }
    }
}
} // namespace
struct FFmpegVideo::Impl {
    int monitor;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> quit{false};
    std::atomic<bool> fallbackRequested{false};
    std::atomic<uint64_t> revision{0};
    uint64_t accepted = 0, workerRevision = 0;
    std::string requested, loaded;
    int64_t seekTarget = 0, position = 0, duration = 0;
    Uint64 anchor = 0;
    bool unloaded = true, ready = false, error = false, eof = false, finished = false, videoStream = true;
    bool valid = false, soft = false, logged = false;
    bool nativeDescriptionLogged = false;
    bool nativePending = false;
    Uint64 nativePendingSinceNs = 0;
    bool needsFrame = true; // independent of the last frame retained across a loop
    bool perspective = false;
    std::array<Point2D, 4> corners{};
    std::vector<uint8_t> warped;
    VideoState target = VideoState::None;
    int loops = 0, playCount = 0;
    VideoDim dim;
    std::deque<Video> videos;
    std::deque<Audio> audios;
    std::thread worker;
    SDL_Texture *texture = nullptr;
    int textureW = 0, textureH = 0;
    SDL_PixelFormat textureFormat = SDL_PIXELFORMAT_UNKNOWN;
    SDL_Colorspace textureColor = SDL_COLORSPACE_UNKNOWN;
    // Software decode normalization happens on the decoder worker. A separate
    // scaler remains render-thread-only for rare hardware-download fallbacks.
    SwsContext *workerScaler = nullptr;
    SwsContext *fallbackScaler = nullptr;
    std::vector<Frame> conversionPool; // worker-owned reusable output frames
    AudioBus::SourceId source = 0;
    std::shared_ptr<AudioBus::Handle> audio;
    AVBufferRef *hardware = nullptr;
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE;
#ifdef RETROFE_HAVE_D3D12
    std::unique_ptr<D3D12VideoInterop> interop;
#endif
#ifdef _WIN32
    std::unique_ptr<D3D11VideoInterop> interop11;
#endif
#ifdef RETROFE_HAVE_EGL_DMABUF
    std::unique_ptr<EGLVideoInterop> egl;
    bool exportDmaBuf = false; // immutable after worker startup
#endif
    SDL_Texture *gpuTexture = nullptr;
    uint64_t gpuFrames = 0;
    // Decoder/demuxer/resampler are exclusively worker-owned.
    AVFormatContext *format = nullptr;
    AVCodecContext *vc = nullptr;
    AVCodecContext *ac = nullptr;
    AVCodecParameters *videoParameters = nullptr;
    AVCodecParameters *audioParameters = nullptr;
    uint64_t videoDecoderReuses = 0;       // exact-configuration reuse
    uint64_t videoDecoderReconfigures = 0; // software H.264/HEVC NEW_EXTRADATA reuse
    uint64_t videoDecoderCreates = 0;
    uint64_t audioDecoderReuses = 0;
    uint64_t audioDecoderCreates = 0;

    // Transactional software H.264/HEVC reconfiguration.
    //
    // videoParameters always describes the configuration the retained decoder
    // has actually accepted. A newly probed configuration lives here until the
    // first packet carrying NEW_EXTRADATA is successfully submitted.
    std::vector<uint8_t> pendingVideoExtradata;
    AVCodecParameters *pendingVideoParameters = nullptr;

    SwrContext *resampler = nullptr;
    AVChannelLayout inputLayout{};
    int inputRate = 0, inputFormat = -1;
    int vi = -1, ai = -1;
    int64_t origin = 0, nextVideo = 0, nextAudio = 0;
    int64_t frameDuration = 33333333; // cached once per opened video stream
    int64_t workerSeek = 0;
    explicit Impl(int m) : monitor(m) {
        installFFmpegLog();
        source = AudioBus::instance().addSource("FFmpeg video");
        audio = AudioBus::instance().getHandle(source);
        if (Configuration::HardwareVideoAccel) {
            SDL_Renderer *r = SDL::getRenderer(m);
            auto shared = acquireSharedHardware(r);
            hardware = shared.deviceCtx;
            hardwareFormat = shared.format;

#ifdef RETROFE_HAVE_D3D12
            if (hardware && hardwareFormat == AV_PIX_FMT_D3D12) {
                interop = std::make_unique<D3D12VideoInterop>(r);
                if (!interop->available()) {
                    interop.reset();
                }
            }
#endif
#ifdef _WIN32
            if (hardware && hardwareFormat == AV_PIX_FMT_D3D11) {
                D3D11VideoInterop::initializeGlobal(r);
                interop11 = std::make_unique<D3D11VideoInterop>(r);
                if (!interop11->available()) {
                    interop11.reset();
                }
            }
#endif
#ifdef RETROFE_HAVE_EGL_DMABUF
            if (hardware && hardwareFormat == AV_PIX_FMT_VAAPI) {
                egl = std::make_unique<EGLVideoInterop>(r);
                exportDmaBuf = egl->available();
                if (!exportDmaBuf)
                    LOG_WARNING("FFmpegVideo", std::string("EGL unavailable; CPU transfer fallback: ") + egl->reason());
            }
#endif
            if (!hardware) {
                LOG_WARNING("FFmpegVideo", "Hardware device unavailable; using software decoding");
            }
        }
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        quit = true;
        ++revision;
        wake.notify_all();
        worker.join();
#ifdef RETROFE_HAVE_EGL_DMABUF
        egl.reset();
#endif
#ifdef RETROFE_HAVE_D3D12
        interop.reset();
#endif
#ifdef _WIN32
        interop11.reset();
#endif
        if (texture && texture != gpuTexture)
            SDL_DestroyTexture(texture);
        av_buffer_unref(&hardware);
        releaseSharedHardware(SDL::getRenderer(monitor));
        sws_freeContext(workerScaler);
        sws_freeContext(fallbackScaler);
        AudioBus::instance().removeSource(source);
    }
    int64_t now() const {
        return position + (target == VideoState::Playing && ready ? int64_t(SDL_GetTicksNS() - anchor) : 0);
    }
    void request(const std::string &path, int64_t seek, bool unload, VideoState state, bool keepFrame = false) {
        keepFrame = keepFrame && !unload && path == requested;
#ifdef RETROFE_HAVE_D3D12
        if (interop && !keepFrame) {
            if (unload)
                interop->discardFrames();
            else
                interop->invalidateFrame();
        }
#endif
#ifdef RETROFE_HAVE_EGL_DMABUF
        if (egl && !keepFrame) egl->discardFrames();
#endif
        std::lock_guard lock(mutex);
        if (requested != path) {
            dim = {};
            nativeDescriptionLogged = false;
        }
        requested = path;
        seekTarget = seek;
        unloaded = unload;
        target = state;
        position = seek;
        anchor = SDL_GetTicksNS();
        ready = false;
        error = false;
        eof = false;
        finished = false;
        if (!keepFrame) { valid = false; nativePending = false; nativePendingSinceNs = 0; }
        needsFrame = true;
        videos.clear();
        audios.clear();
        ++revision;
        AudioBus::instance().clear(audio);
        wake.notify_all();
    }
    void clearPendingVideoReconfiguration() {
        pendingVideoExtradata.clear();
        avcodec_parameters_free(&pendingVideoParameters);
    }

    void commitPendingVideoReconfiguration(const std::string &path) {
        if (!pendingVideoParameters)
            return;

        // avcodec_send_packet() has accepted the packet carrying
        // NEW_EXTRADATA. Only now does our retained-configuration model advance
        // to the new stream.
        avcodec_parameters_free(&videoParameters);
        videoParameters = pendingVideoParameters;
        pendingVideoParameters = nullptr;
        pendingVideoExtradata.clear();
        ++videoDecoderReconfigures;

        LOG_INFO(
            "FFmpegVideo",
            "Committed video decoder reconfiguration (" +
                std::string(vc && vc->codec ? vc->codec->name : "unknown") +
                ") for " + path + " after NEW_EXTRADATA packet acceptance");

        LOG_DEBUG(
            "FFmpegVideo",
            "Decoder reuse totals after commit: video exact=" +
                std::to_string(videoDecoderReuses) +
                " reconfigured=" +
                std::to_string(videoDecoderReconfigures) +
                " created=" + std::to_string(videoDecoderCreates) +
                "; audio reused=" + std::to_string(audioDecoderReuses) +
                " created=" + std::to_string(audioDecoderCreates));
    }

    void closeMedia(bool retainDecoders = false) {
        // A pending configuration belongs to exactly one URI transition. If
        // that transition is superseded before its first video packet is
        // accepted, discard the candidate and leave videoParameters describing
        // the decoder's previously committed configuration.
        clearPendingVideoReconfiguration();

        if (!retainDecoders) {
            avcodec_free_context(&vc);
            avcodec_parameters_free(&videoParameters);
            avcodec_free_context(&ac);
            avcodec_parameters_free(&audioParameters);
        }
        avformat_close_input(&format);
        swr_free(&resampler);
        av_channel_layout_uninit(&inputLayout);
        vi = ai = -1;
        loaded.clear();
    }
    AVCodecContext *decoder(int index) {
        if (index < 0)
            return nullptr;
        auto *codec = avcodec_find_decoder(format->streams[index]->codecpar->codec_id);
        if (!codec)
            throw std::runtime_error("Decoder unavailable");
        auto *c = avcodec_alloc_context3(codec);
        if (!c)
            throw std::bad_alloc();
        try {
            check(avcodec_parameters_to_context(c, format->streams[index]->codecpar), "Codec parameters");
            c->pkt_timebase = format->streams[index]->time_base;
            if (index == vi) {
                c->thread_count = std::max(0, Configuration::AvdecMaxThreads);
                c->thread_type = Configuration::AvdecThreadType;
            }
            if (index == vi && hardware) {
                for (int i = 0; const auto *config = avcodec_get_hw_config(codec, i); ++i) {
                    if (config->pix_fmt == hardwareFormat &&
                        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                        c->hw_device_ctx = av_buffer_ref(hardware);
                        c->thread_count = 1;
                        c->opaque = this;
                        c->get_format = [](AVCodecContext *c, const AVPixelFormat *formats) {
                            auto desired = static_cast<Impl *>(c->opaque)->hardwareFormat;
                            for (auto *f = formats; *f != AV_PIX_FMT_NONE; ++f)
                                if (*f == desired)
                                    return *f;
                            for (auto *f = formats; *f != AV_PIX_FMT_NONE; ++f)
                                if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL))
                                    return *f;
                            return AV_PIX_FMT_NONE;
                        };
                        break;
                    }
                }
            }
            int openRc = avcodec_open2(c, codec, nullptr);
            if (openRc < 0 && c->hw_device_ctx) {
                LOG_WARNING("FFmpegVideo", "Hardware decoder initialization failed (" + std::to_string(openRc) + "); retrying with software decoding");
                av_buffer_unref(&c->hw_device_ctx);
                c->get_format = nullptr;
                c->opaque = nullptr;
                c->thread_count = std::max(0, Configuration::AvdecMaxThreads);
                c->thread_type = Configuration::AvdecThreadType;
                openRc = avcodec_open2(c, codec, nullptr);
            }
            check(openRc, "Open decoder");
        } catch (...) {
            avcodec_free_context(&c);
            throw;
        }
        return c;
    }
    void fallbackToSoftwareVideo() {
        if (hardware) {
            av_buffer_unref(&hardware);
        }
        hardwareFormat = AV_PIX_FMT_NONE;
        fallbackRequested = false;
        avcodec_free_context(&vc);
        avcodec_parameters_free(&videoParameters);
        clearPendingVideoReconfiguration();
        try {
            vc = decoder(vi);
            if (format && vi >= 0) {
                int64_t currentPts = nextVideo;
                if (currentPts > 0 && format->streams[vi]->time_base.den > 0) {
                    int64_t targetTs = av_rescale_q(currentPts, AVRational{1, 1000000000}, format->streams[vi]->time_base);
                    av_seek_frame(format, vi, targetTs, AVSEEK_FLAG_BACKWARD);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("FFmpegVideo", std::string("Software video fallback failed: ") + e.what());
        }
    }
    Frame acquireConversionFrame(AVPixelFormat format, int width, int height) {
        for (auto &candidate : conversionPool) {
            // The pool keeps one reference. If that's the only reference, the
            // render queue no longer owns this frame and its buffers can be reused.
            if (candidate.use_count() != 1)
                continue;

            AVFrame *out = candidate.get();
            if (out->format != format || out->width != width || out->height != height || !out->buf[0]) {
                av_frame_unref(out);
                out->format = format;
                out->width = width;
                out->height = height;
                check(av_frame_get_buffer(out, 32), "Allocate software conversion frame");
            } else {
                check(av_frame_make_writable(out), "Make software conversion frame writable");
            }
            return candidate;
        }

        Frame out = frame();
        if (!out)
            throw std::bad_alloc();
        out->format = format;
        out->width = width;
        out->height = height;
        check(av_frame_get_buffer(out.get(), 32), "Allocate software conversion frame");
        conversionPool.push_back(out);
        return out;
    }

    Frame prepareSoftwareFrame(
        Frame f,
        bool usePerspective,
        const std::array<Point2D, 4> &warpCorners)
    {
        if (!f)
            return f;

        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        if (f->hw_frames_ctx || (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)))
            return f;

        check(av_frame_apply_cropping(f.get(), 0), "Apply decoded crop metadata");

        const AVPixelFormat output =
            usePerspective ? AV_PIX_FMT_RGBA : AV_PIX_FMT_YUV420P;

        Frame normalized = f;
        if (f->format != output) {
            Frame converted = acquireConversionFrame(output, f->width, f->height);

            // The reusable pool deliberately keeps only the metadata this backend
            // consumes. Avoid cloning frame side-data onto a reused AVFrame.
            converted->color_range = f->color_range;
            converted->color_primaries = f->color_primaries;
            converted->color_trc = f->color_trc;
            converted->colorspace = f->colorspace;
            converted->chroma_location = f->chroma_location;
            converted->sample_aspect_ratio = f->sample_aspect_ratio;

            workerScaler = sws_getCachedContext(
                workerScaler,
                f->width, f->height, AVPixelFormat(f->format),
                f->width, f->height, output,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!workerScaler)
                throw std::runtime_error("Create software conversion scaler");

            const int matrix =
                f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
                f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 :
                (f->colorspace == AVCOL_SPC_BT470BG ||
                 f->colorspace == AVCOL_SPC_SMPTE170M) ? SWS_CS_ITU601 :
                SWS_CS_ITU709;

            const int *coefficients = sws_getCoefficients(matrix);
            const int range = f->color_range == AVCOL_RANGE_JPEG;
            sws_setColorspaceDetails(
                workerScaler,
                coefficients, range,
                coefficients, usePerspective ? 1 : range,
                0, 1 << 16, 1 << 16);

            const int rows = sws_scale(
                workerScaler,
                f->data, f->linesize,
                0, f->height,
                converted->data, converted->linesize);
            if (rows != f->height)
                throw std::runtime_error("Incomplete software frame conversion");

            normalized = std::move(converted);
        }

        if (usePerspective)
            return warpSoftwareFrame(std::move(normalized), warpCorners);

        return normalized;
    }

    Frame warpSoftwareFrame(
        Frame rgba,
        const std::array<Point2D, 4> &warpCorners)
    {
        if (!rgba || rgba->format != AV_PIX_FMT_RGBA)
            throw std::runtime_error("Perspective source is not RGBA");

        Frame warpedFrame =
            acquireConversionFrame(AV_PIX_FMT_RGBA, rgba->width, rgba->height);

        // acquireConversionFrame() may return the same pooled frame if rgba did
        // not come from the pool, but never a frame still referenced by rgba.
        if (warpedFrame.get() == rgba.get()) {
            Frame replacement = frame();
            if (!replacement)
                throw std::bad_alloc();
            replacement->format = AV_PIX_FMT_RGBA;
            replacement->width = rgba->width;
            replacement->height = rgba->height;
            check(av_frame_get_buffer(replacement.get(), 32), "Allocate perspective output frame");
            conversionPool.push_back(replacement);
            warpedFrame = std::move(replacement);
        }

        check(av_frame_make_writable(warpedFrame.get()), "Make perspective output writable");

        const auto h =
            computePerspectiveMatrixFromCorners(
                rgba->width,
                rgba->height,
                warpCorners);

        for (int y = 0; y < warpedFrame->height; ++y)
            std::memset(
                warpedFrame->data[0] + size_t(y) * warpedFrame->linesize[0],
                0,
                size_t(warpedFrame->width) * 4);

        for (int y = 0; y < rgba->height; ++y) {
            uint8_t *dstRow =
                warpedFrame->data[0] + size_t(y) * warpedFrame->linesize[0];

            for (int x = 0; x < rgba->width; ++x) {
                const double z = h[6] * x + h[7] * y + h[8];
                if (std::abs(z) < 1e-9)
                    continue;

                const double sx = (h[0] * x + h[1] * y + h[2]) / z;
                const double sy = (h[3] * x + h[4] * y + h[5]) / z;
                if (sx < 0 || sy < 0 || sx >= rgba->width || sy >= rgba->height)
                    continue;

                const int ix = int(sx);
                const int iy = int(sy);
                const int jx = std::min(ix + 1, rgba->width - 1);
                const int jy = std::min(iy + 1, rgba->height - 1);
                const double dx = sx - ix;
                const double dy = sy - iy;

                const uint8_t *row0 =
                    rgba->data[0] + size_t(iy) * rgba->linesize[0];
                const uint8_t *row1 =
                    rgba->data[0] + size_t(jy) * rgba->linesize[0];
                uint8_t *dst = dstRow + size_t(x) * 4;

                for (int c = 0; c < 4; ++c) {
                    dst[c] = uint8_t(
                        (1 - dy) *
                            ((1 - dx) * row0[ix * 4 + c] +
                             dx * row0[jx * 4 + c]) +
                        dy *
                            ((1 - dx) * row1[ix * 4 + c] +
                             dx * row1[jx * 4 + c]));
                }
            }
        }

        warpedFrame->color_range = rgba->color_range;
        warpedFrame->color_primaries = rgba->color_primaries;
        warpedFrame->color_trc = rgba->color_trc;
        warpedFrame->colorspace = rgba->colorspace;
        warpedFrame->chroma_location = rgba->chroma_location;
        warpedFrame->sample_aspect_ratio = rgba->sample_aspect_ratio;
        return warpedFrame;
    }

    void pumpAudioDue() {
        bool freed = false;

        {
            // Keep AudioBus delivery serialized with pause()/request(), just as
            // it was when updateFrame() performed the push while holding mutex.
            std::lock_guard lock(mutex);
            if (!ready || unloaded || target != VideoState::Playing || audios.empty())
                return;

            const int64_t time = now();
            while (!audios.empty() && audios.front().pts <= time + 20000000) {
                Audio a = std::move(audios.front());
                audios.pop_front();
                freed = true;

                // Preserve the old policy: very late audio is discarded.
                if (a.pts >= time - 100000000) {
                    AudioBus::instance().push(
                        audio,
                        a.data.data(),
                        int(a.data.size() * sizeof(float)));
                }
            }
        }

        if (freed)
            wake.notify_all();
    }

    static uint64_t parameterBytesHash(const uint8_t *data, int size) {
        // FNV-1a is only for compact diagnostics; this is not a security hash.
        uint64_t hash = 1469598103934665603ull;
        for (int i = 0; data && i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    static std::string rationalText(AVRational value) {
        return std::to_string(value.num) + "/" + std::to_string(value.den);
    }

    bool compatibleVideo(
        const AVCodecParameters &b,
        std::string *mismatchReport = nullptr) const
    {
        if (!vc || !videoParameters) {
            if (mismatchReport)
                *mismatchReport = "no retained video decoder/configuration";
            return false;
        }

        const auto &a = *videoParameters;
        bool compatible = true;
        std::ostringstream report;
        bool first = true;

        auto mismatch = [&](const std::string &message) {
            compatible = false;
            if (!mismatchReport)
                return;
            if (!first)
                report << "; ";
            report << message;
            first = false;
        };

        auto changed = [&](const char *name, auto oldValue, auto newValue) {
            if (oldValue != newValue) {
                mismatch(
                    std::string(name) + " " +
                    std::to_string(oldValue) + " -> " +
                    std::to_string(newValue));
            }
        };

        changed("codec_type", a.codec_type, b.codec_type);
        changed("codec_id", a.codec_id, b.codec_id);
        changed("codec_tag", a.codec_tag, b.codec_tag);
        changed("format", a.format, b.format);

        if (a.width != b.width || a.height != b.height) {
            mismatch(
                "resolution " +
                std::to_string(a.width) + "x" + std::to_string(a.height) +
                " -> " +
                std::to_string(b.width) + "x" + std::to_string(b.height));
        }

        changed("profile", a.profile, b.profile);
        changed("level", a.level, b.level);
        changed(
            "bits_per_coded_sample",
            a.bits_per_coded_sample,
            b.bits_per_coded_sample);
        changed(
            "bits_per_raw_sample",
            a.bits_per_raw_sample,
            b.bits_per_raw_sample);
        changed("field_order", a.field_order, b.field_order);
        changed("video_delay", a.video_delay, b.video_delay);
        changed("color_range", a.color_range, b.color_range);
        changed("color_primaries", a.color_primaries, b.color_primaries);
        changed("color_trc", a.color_trc, b.color_trc);
        changed("color_space", a.color_space, b.color_space);
        changed("chroma_location", a.chroma_location, b.chroma_location);
        changed("alpha_mode", a.alpha_mode, b.alpha_mode);

        if (av_cmp_q(a.sample_aspect_ratio, b.sample_aspect_ratio) != 0) {
            mismatch(
                "sample_aspect_ratio " +
                rationalText(a.sample_aspect_ratio) + " -> " +
                rationalText(b.sample_aspect_ratio));
        }

        if (av_cmp_q(a.framerate, b.framerate) != 0) {
            mismatch(
                "framerate " +
                rationalText(a.framerate) + " -> " +
                rationalText(b.framerate));
        }

        if (a.nb_coded_side_data != b.nb_coded_side_data) {
            mismatch(
                "coded_side_data_count " +
                std::to_string(a.nb_coded_side_data) + " -> " +
                std::to_string(b.nb_coded_side_data));
        } else if (a.nb_coded_side_data != 0) {
            // We deliberately do not attempt decoder reuse when coded side data
            // is present because compatibleVideo() does not deeply compare it.
            mismatch(
                "coded_side_data present (" +
                std::to_string(a.nb_coded_side_data) + ")");
        }

        if (a.extradata_size <= 0 || b.extradata_size <= 0) {
            mismatch(
                "extradata unavailable (" +
                std::to_string(a.extradata_size) + " -> " +
                std::to_string(b.extradata_size) + " bytes)");
        } else if (a.extradata_size != b.extradata_size) {
            mismatch(
                "extradata_size " +
                std::to_string(a.extradata_size) + " -> " +
                std::to_string(b.extradata_size));
        } else if (!a.extradata || !b.extradata) {
            mismatch("extradata pointer missing despite nonzero size");
        } else if (std::memcmp(
                       a.extradata,
                       b.extradata,
                       static_cast<size_t>(a.extradata_size)) != 0)
        {
            std::ostringstream hashes;
            hashes << "extradata differs ("
                   << a.extradata_size << " bytes, hash 0x"
                   << std::hex << parameterBytesHash(a.extradata, a.extradata_size)
                   << " -> 0x"
                   << parameterBytesHash(b.extradata, b.extradata_size)
                   << std::dec << ")";
            mismatch(hashes.str());
        }

        if (mismatchReport)
            *mismatchReport = report.str();

        return compatible;
    }

    bool reconfigurableSoftwareVideo(
        const AVCodecParameters &b,
        std::string *rejectionReason = nullptr) const
    {
        if (!vc || !videoParameters) {
            if (rejectionReason)
                *rejectionReason = "no retained video decoder/configuration";
            return false;
        }

        const auto &a = *videoParameters;
        std::ostringstream reason;
        bool first = true;
        bool okay = true;

        auto reject = [&](const std::string &message) {
            okay = false;
            if (!rejectionReason)
                return;
            if (!first)
                reason << "; ";
            reason << message;
            first = false;
        };

        // Keep this path software-only. Hardware decoder reconfiguration has
        // vendor/driver lifecycle requirements that are intentionally left on
        // the exact-match/recreate path.
        if (vc->hw_device_ctx)
            reject("hardware decoder context");

        if (a.codec_type != AVMEDIA_TYPE_VIDEO ||
            b.codec_type != AVMEDIA_TYPE_VIDEO)
        {
            reject("not a video stream");
        }

        if (a.codec_id != b.codec_id)
            reject("codec changed");

        if (b.codec_id != AV_CODEC_ID_H264 &&
            b.codec_id != AV_CODEC_ID_HEVC)
        {
            reject("codec does not support this reconfiguration tier");
        }

        // These describe the decoded surface contract we want to keep stable.
        if (a.width != b.width || a.height != b.height) {
            reject(
                "resolution " +
                std::to_string(a.width) + "x" + std::to_string(a.height) +
                " -> " +
                std::to_string(b.width) + "x" + std::to_string(b.height));
        }

        if (a.format != b.format)
            reject("decoded format expectation changed");

        if (a.bits_per_coded_sample != b.bits_per_coded_sample)
            reject("bits_per_coded_sample changed");

        if (a.bits_per_raw_sample != b.bits_per_raw_sample)
            reject("bits_per_raw_sample changed");

        if (a.field_order != b.field_order)
            reject("field_order changed");

        // We do not deeply compare arbitrary coded side data here.
        if (a.nb_coded_side_data || b.nb_coded_side_data)
            reject("coded side data present");

        // The supported handoff mechanism needs new global headers to attach
        // to the first packet of the new stream.
        if (!b.extradata || b.extradata_size <= 0)
            reject("new stream has no extradata");

        if (rejectionReason)
            *rejectionReason = reason.str();

        return okay;
    }

    bool compatibleAudio(const AVCodecParameters &b) const {
        if (!ac || !audioParameters)
            return false;

        const auto &a = *audioParameters;

        // Keep audio reuse deliberately conservative for the same reason as
        // video reuse: an already-open AVCodecContext is reused only when the
        // decoder configuration is effectively identical. AAC-in-MP4, the
        // common artwork case, normally carries the AudioSpecificConfig in
        // extradata and therefore gets a strong compatibility check here.
        if (a.extradata_size <= 0 ||
            a.extradata_size != b.extradata_size ||
            a.nb_coded_side_data ||
            b.nb_coded_side_data)
        {
            return false;
        }

        return a.codec_type == b.codec_type &&
            a.codec_id == b.codec_id &&
            a.codec_tag == b.codec_tag &&
            a.format == b.format &&
            a.profile == b.profile &&
            a.level == b.level &&
            a.bits_per_coded_sample == b.bits_per_coded_sample &&
            a.bits_per_raw_sample == b.bits_per_raw_sample &&
            a.sample_rate == b.sample_rate &&
            a.block_align == b.block_align &&
            a.frame_size == b.frame_size &&
            a.initial_padding == b.initial_padding &&
            a.trailing_padding == b.trailing_padding &&
            a.seek_preroll == b.seek_preroll &&
            av_channel_layout_compare(&a.ch_layout, &b.ch_layout) == 0 &&
            std::memcmp(a.extradata, b.extradata, a.extradata_size) == 0;
    }

    void retainParameters(
        AVCodecParameters *&destination,
        const AVCodecParameters *source,
        const char *where)
    {
        avcodec_parameters_free(&destination);
        if (!source)
            return;

        destination = avcodec_parameters_alloc();
        if (!destination)
            throw std::bad_alloc();

        try {
            check(avcodec_parameters_copy(destination, source), where);
        } catch (...) {
            avcodec_parameters_free(&destination);
            throw;
        }
    }
    void load(const std::string &path, int64_t seek) {
        const bool fresh = loaded != path || !format;
        if (fresh) {
            // Close the old container/source but retain both codec contexts just
            // long enough to compare them against the newly probed streams.
            closeMedia(true);
            format = avformat_alloc_context();
            if (!format)
                throw std::bad_alloc();
            format->interrupt_callback = {[](void *p) {
                                              auto &s = *static_cast<Impl *>(p);
                                              return int(s.quit || s.revision != s.workerRevision);
                                          },
                                          this};
            check(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "Open media");
            check(avformat_find_stream_info(format, nullptr), "Read stream information");
            vi = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            ai = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (vi < 0 && ai < 0)
                throw std::runtime_error("No playable streams");

            frameDuration = 33333333;
            if (vi >= 0) {
                const AVRational rate = av_guess_frame_rate(format, format->streams[vi], nullptr);
                if (rate.num > 0 && rate.den > 0)
                    frameDuration = av_rescale_q(1, av_inv_q(rate), ns);
            }

            const bool hadRetainedVideo =
                vc != nullptr &&
                videoParameters != nullptr;

            const AVCodecParameters *newVideoParameters =
                vi >= 0 ? format->streams[vi]->codecpar : nullptr;

            std::string videoReuseMismatch;
            const bool reusedVideoExact =
                newVideoParameters &&
                compatibleVideo(
                    *newVideoParameters,
                    hadRetainedVideo ? &videoReuseMismatch : nullptr);

            std::string reconfigureRejection;
            const bool reusedVideoReconfigured =
                !reusedVideoExact &&
                hadRetainedVideo &&
                newVideoParameters &&
                reconfigurableSoftwareVideo(
                    *newVideoParameters,
                    &reconfigureRejection);

            if (!reusedVideoExact &&
                hadRetainedVideo &&
                newVideoParameters &&
                !videoReuseMismatch.empty())
            {
                if (reusedVideoReconfigured) {
                    LOG_DEBUG(
                        "FFmpegVideo",
                        "Exact video decoder reuse rejected for " + path +
                        ": " + videoReuseMismatch +
                        "; using software NEW_EXTRADATA reconfiguration");
                } else {
                    LOG_DEBUG(
                        "FFmpegVideo",
                        "Video decoder reuse rejected for " + path +
                        ": " + videoReuseMismatch +
                        (reconfigureRejection.empty()
                            ? std::string()
                            : "; reconfiguration rejected: " +
                              reconfigureRejection));
                }
            }

            if (reusedVideoExact) {
                // Tier 1: exact-compatible stream. Release old reference frames
                // while keeping the already-open decoder and hardware device.
                avcodec_flush_buffers(vc);
                vc->pkt_timebase = format->streams[vi]->time_base;
                clearPendingVideoReconfiguration();
                ++videoDecoderReuses;
            } else if (reusedVideoReconfigured) {
                // Tier 2: software H.264/HEVC can accept new global headers on
                // the first packet via AV_PKT_DATA_NEW_EXTRADATA. Flush all
                // old decode/reorder state, retain the open codec context, and
                // queue the newly probed avcC/hvcC blob for that first packet.
                avcodec_flush_buffers(vc);
                vc->pkt_timebase = format->streams[vi]->time_base;

                clearPendingVideoReconfiguration();

                pendingVideoExtradata.assign(
                    newVideoParameters->extradata,
                    newVideoParameters->extradata +
                        newVideoParameters->extradata_size);

                retainParameters(
                    pendingVideoParameters,
                    newVideoParameters,
                    "Retain pending reconfigured video configuration");
            } else {
                // Tier 3: incompatible, different codec, unsupported dynamic
                // configuration, or no prior decoder. Recreate only the codec
                // context; the platform AVHWDeviceContext remains warm.
                clearPendingVideoReconfiguration();
                avcodec_free_context(&vc);
                avcodec_parameters_free(&videoParameters);
                vc = decoder(vi);
                if (vc) {
                    retainParameters(
                        videoParameters,
                        format->streams[vi]->codecpar,
                        "Retain video configuration");
                    ++videoDecoderCreates;
                }
            }

            const bool reusedAudio =
                ai >= 0 &&
                compatibleAudio(*format->streams[ai]->codecpar);

            if (reusedAudio) {
                avcodec_flush_buffers(ac);
                ac->pkt_timebase = format->streams[ai]->time_base;
                ++audioDecoderReuses;
            } else {
                avcodec_free_context(&ac);
                avcodec_parameters_free(&audioParameters);
                ac = decoder(ai);
                if (ac) {
                    retainParameters(
                        audioParameters,
                        format->streams[ai]->codecpar,
                        "Retain audio configuration");
                    ++audioDecoderCreates;
                }
            }

            loaded = path;
            origin = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time * 1000;

            if (vc) {
                if (reusedVideoExact) {
                    LOG_INFO(
                        "FFmpegVideo",
                        "Reused video decoder (" +
                            std::string(vc->codec->name) +
                            ") for " + path);
                } else if (reusedVideoReconfigured) {
                    LOG_INFO(
                        "FFmpegVideo",
                        "Prepared video decoder reconfiguration (" +
                            std::string(vc->codec->name) +
                            ") for " + path +
                            " using NEW_EXTRADATA; commit pending first video packet");
                } else {
                    LOG_INFO(
                        "FFmpegVideo",
                        "Created video decoder (" +
                            std::string(vc->codec->name) +
                            ") for " + path);
                }
            } else {
                LOG_INFO("FFmpegVideo", "No video decoder for " + path);
            }

            if (ac) {
                LOG_INFO(
                    "FFmpegVideo",
                    std::string(reusedAudio ? "Reused" : "Created") +
                    " audio decoder (" + ac->codec->name + ") for " + path);
            } else {
                LOG_INFO("FFmpegVideo", "No audio decoder for " + path);
            }

            LOG_DEBUG(
                "FFmpegVideo",
                "Decoder reuse totals (committed): video exact=" +
                    std::to_string(videoDecoderReuses) +
                    " reconfigured=" +
                    std::to_string(videoDecoderReconfigures) +
                    " created=" + std::to_string(videoDecoderCreates) +
                    "; audio reused=" + std::to_string(audioDecoderReuses) +
                    " created=" + std::to_string(audioDecoderCreates));

            LOG_INFO(
                "FFmpegVideo",
                "Opened " + path +
                    "; video decoder=" + (vc ? vc->codec->name : "none") +
                    "; audio decoder=" + (ac ? ac->codec->name : "none"));
        }
        // Same-file reopen retains the demuxer and codec contexts.
        // Stream probing retains initial packets. Keep them on a cold open.
        if (seek || !fresh) {
            check(avformat_seek_file(format, -1, INT64_MIN, (seek + origin) / 1000, INT64_MAX, 0),
                  "Seek media");
            if (vc)
                avcodec_flush_buffers(vc);
            if (ac)
                avcodec_flush_buffers(ac);
        }
        swr_free(&resampler);
        nextVideo = nextAudio = seek;
        workerSeek = seek;
    }
    bool publish(Frame f, bool video, uint64_t rev) {
        const auto *stream = format->streams[video ? vi : ai];
        int64_t pts = f->best_effort_timestamp == AV_NOPTS_VALUE
                          ? (video ? nextVideo : nextAudio)
                          : av_rescale_q(f->best_effort_timestamp, stream->time_base, ns) - origin;
        if (video) {
            nextVideo = pts + frameDuration;
            if (pts < workerSeek)
                return true;

            bool usePerspective = false;
            std::array<Point2D, 4> useCorners{};
            {
                std::lock_guard lock(mutex);
                usePerspective = perspective;
                useCorners = corners;
            }

            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
            const bool softwareFrame =
                !f->hw_frames_ctx &&
                !(desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL));

            if (softwareFrame)
                f = prepareSoftwareFrame(std::move(f), usePerspective, useCorners);

#ifdef RETROFE_HAVE_EGL_DMABUF
            Frame mapped;
            if (exportDmaBuf && f->format == AV_PIX_FMT_VAAPI) {
                mapped = frame();
                if (mapped) {
                    mapped->format = AV_PIX_FMT_DRM_PRIME;
                    // READ synchronizes the producer on this worker; DIRECT forbids pixel copies.
                    if (av_hwframe_map(mapped.get(), f.get(), AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0)
                        mapped.reset();
                }
            }
#endif
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return quit || revision != rev || videos.size() < 3; });
            if (quit || revision != rev)
                return false;
            videos.push_back({
                std::move(f),
                std::max<int64_t>(0, pts),
                softwareFrame,
                usePerspective
#ifdef RETROFE_HAVE_EGL_DMABUF
                , std::move(mapped)
#endif
            });
        } else {
            if (!resampler || inputRate != f->sample_rate || inputFormat != f->format ||
                av_channel_layout_compare(&inputLayout, &f->ch_layout)) {
                swr_free(&resampler);
                av_channel_layout_uninit(&inputLayout);
                check(av_channel_layout_copy(&inputLayout, &f->ch_layout), "Copy audio layout");
                inputRate = f->sample_rate;
                inputFormat = f->format;
                AVChannelLayout out;
                av_channel_layout_default(&out, AudioBus::instance().dev_channels());
                int rc =
                    swr_alloc_set_opts2(&resampler, &out, AV_SAMPLE_FMT_FLT, AudioBus::instance().dev_rate(),
                                        &f->ch_layout, AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&out);
                check(rc, "Create audio resampler");
                check(swr_init(resampler), "Initialize resampler");
            }
            const int channels = AudioBus::instance().dev_channels();
            int count = swr_get_out_samples(resampler, f->nb_samples);
            check(count, "Calculate resampled size");
            Audio a;
            a.pts = std::max<int64_t>(0, pts);
            a.data.resize(size_t(count) * channels);
            uint8_t *out = reinterpret_cast<uint8_t *>(a.data.data());
            count = swr_convert(resampler, &out, count, const_cast<const uint8_t **>(f->extended_data),
                                f->nb_samples);
            check(count, "Resample audio");
            a.data.resize(size_t(count) * channels);
            nextAudio = pts + int64_t(count) * 1000000000 / AudioBus::instance().dev_rate();
            if (pts < workerSeek)
                return true;
            std::unique_lock lock(mutex);
            // Avoid starving video preroll behind interleaved audio packets.
            if (audios.size() == 32)
                audios.pop_front();
            if (quit || revision != rev)
                return false;
            audios.push_back(std::move(a));
        }
        return true;
    }
    bool receive(AVCodecContext *c, bool video, uint64_t rev) {
        for (;;) {
            auto f = frame();
            if (!f)
                throw std::bad_alloc();
            int rc = avcodec_receive_frame(c, f.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                return true;
            if (rc < 0 && video && c->hw_device_ctx) {
                LOG_WARNING("FFmpegVideo", "Hardware decode error receiving frame (" + std::to_string(rc) + "); falling back to software decoding");
                fallbackToSoftwareVideo();
                return false;
            }
            check(rc, "Decode frame");
            if (!publish(std::move(f), video, rev))
                return false;
        }
    }
    void run() {
        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            std::lock_guard lock(mutex);
            error = true;
            return;
        }
        while (!quit) {
            // Audio is clocked against the same presentation timeline as before,
            // but delivery now happens on the worker instead of the render thread.
            pumpAudioDue();

            std::string path;
            int64_t seek = 0;
            uint64_t rev;
            bool idle;
            {
                std::unique_lock lock(mutex);

                auto canWork = [&] {
                    return quit || fallbackRequested.load(std::memory_order_acquire) || revision != accepted ||
                           (!unloaded && !error && !eof && ready &&
                            (target == VideoState::Playing ||
                             (vi >= 0 ? videos.empty() && needsFrame : audios.empty())) &&
                            videos.size() < 3 && (vi >= 0 || audios.size() < 32));
                };

                if (!canWork()) {
                    if (ready && !unloaded && target == VideoState::Playing && !audios.empty()) {
                        const int64_t untilAudio =
                            audios.front().pts - (now() + 20000000);
                        if (untilAudio > 0) {
                            wake.wait_for(
                                lock,
                                std::chrono::nanoseconds(untilAudio),
                                canWork);
                        }
                    } else {
                        wake.wait(lock, canWork);
                    }
                }

                if (quit)
                    break;

                // A timed wait can expire solely because the next audio chunk is
                // due. Loop back so pumpAudioDue() handles it before demuxing more.
                if (!canWork())
                    continue;

                rev = revision;
                path = requested;
                seek = seekTarget;
                idle = unloaded;
            }
            workerRevision = rev;
            ffmpegMedia = path;
            try {
                if (fallbackRequested.exchange(false, std::memory_order_acq_rel)) {
                    LOG_INFO("FFmpegVideo", "Performing software video fallback on worker thread");
                    fallbackToSoftwareVideo();
                }
                if (rev != accepted) {
                    if (path.empty())
                        closeMedia();
                    else if (!idle)
                        load(path, seek);
                    std::lock_guard lock(mutex);
                    if (revision != rev)
                        continue;
                    accepted = rev;
                    ready = !unloaded && !path.empty();
                    videoStream = vi >= 0;
                    duration = format && format->duration != AV_NOPTS_VALUE
                                   ? std::max<int64_t>(0, format->duration * 1000)
                                   : 0;
                    anchor = SDL_GetTicksNS();
                    if (!ready)
                        continue;
                }
                av_packet_unref(packet);
                int rc = av_read_frame(format, packet);
                if (rc == AVERROR_EOF) {
                    if (vc) {
                        check(avcodec_send_packet(vc, nullptr), "Drain video");
                        receive(vc, true, rev);
                    }
                    if (ac) {
                        check(avcodec_send_packet(ac, nullptr), "Drain audio");
                        receive(ac, false, rev);
                    }
                    std::lock_guard lock(mutex);
                    if (revision == rev)
                        eof = true;
                    continue;
                }
                check(rc, "Read packet");

                const bool committingVideoReconfiguration =
                    packet->stream_index == vi &&
                    pendingVideoParameters != nullptr &&
                    !pendingVideoExtradata.empty();

                if (committingVideoReconfiguration) {
                    size_t existingSize = 0;
                    const uint8_t *existing =
                        av_packet_get_side_data(
                            packet,
                            AV_PKT_DATA_NEW_EXTRADATA,
                            &existingSize);

                    if (!existing) {
                        uint8_t *side =
                            av_packet_new_side_data(
                                packet,
                                AV_PKT_DATA_NEW_EXTRADATA,
                                pendingVideoExtradata.size());
                        if (!side)
                            throw std::bad_alloc();

                        std::memcpy(
                            side,
                            pendingVideoExtradata.data(),
                            pendingVideoExtradata.size());

                        LOG_DEBUG(
                            "FFmpegVideo",
                            "Attached " +
                                std::to_string(
                                    pendingVideoExtradata.size()) +
                                " bytes of NEW_EXTRADATA to first video packet for " +
                                path);
                    } else {
                        // A demuxer-supplied update is authoritative for this
                        // packet; don't add a duplicate side-data entry.
                        LOG_DEBUG(
                            "FFmpegVideo",
                            "First video packet already carries " +
                                std::to_string(existingSize) +
                                " bytes of NEW_EXTRADATA; using demuxer-provided data for " +
                                path);
                    }
                }

                AVCodecContext *c = packet->stream_index == vi   ? vc
                                    : packet->stream_index == ai ? ac
                                                                 : nullptr;
                if (c) {
                    rc = avcodec_send_packet(c, packet);
                    if (rc == AVERROR(EAGAIN)) {
                        if (!receive(c, c == vc, rev))
                            continue;
                        rc = avcodec_send_packet(c, packet);
                    }
                    if (rc < 0 && c == vc && vc->hw_device_ctx) {
                        LOG_WARNING("FFmpegVideo", "Hardware decode error during packet submission (" + std::to_string(rc) + "); falling back to software decoding");
                        fallbackToSoftwareVideo();
                        continue;
                    }
                    check(rc, "Submit packet");

                    // The decoder has now accepted the packet carrying the new
                    // global headers. Commit our model before receiving output,
                    // because from this point forward the decoder itself has
                    // advanced even if receive() later reports a stream error.
                    if (committingVideoReconfiguration)
                        commitPendingVideoReconfiguration(path);

                    if (!receive(c, c == vc, rev))
                        continue;
                }
            } catch (const std::exception &e) {
                // If a retarget was superseded while open/probe was in flight,
                // keep whichever decoder contexts are currently warm. The next
                // requested URI will run the same conservative compatibility
                // checks before either context is reused. Real failures on the
                // active request still discard everything.
                const bool supersededTransition =
                    accepted != rev &&
                    revision.load(std::memory_order_acquire) != rev;

                closeMedia(supersededTransition);

                std::lock_guard lock(mutex);
                if (revision == rev) {
                    error = true;
                    ready = false;
                    accepted = rev;
                    LOG_ERROR("FFmpegVideo", path + ": " + e.what());
                } else if (supersededTransition) {
                    LOG_DEBUG(
                        "FFmpegVideo",
                        "Superseded URI transition aborted; retained decoder contexts for next target");
                }
            }
        }
        av_packet_free(&packet);
        closeMedia();
    }
};
FFmpegVideo::FFmpegVideo(int m) : impl_(std::make_unique<Impl>(m)) {}
FFmpegVideo::~FFmpegVideo() = default;
bool FFmpegVideo::initialize() { return true; }
bool FFmpegVideo::deInitialize() { return stop(); }
bool FFmpegVideo::open(const std::string &f) {
    impl_->playCount = 0;
    impl_->logged = false;
    impl_->request(f, 0, false, VideoState::Paused);
    return true;
}
bool FFmpegVideo::unload() {
    impl_->request(impl_->requested, 0, true, VideoState::None);
    return true;
}
bool FFmpegVideo::stop() {
    impl_->request("", 0, true, VideoState::None);
    return true;
}
bool FFmpegVideo::prepareForRetarget() { return unload(); }
bool FFmpegVideo::isReadyForReuse() const {
    auto &p = *impl_;
    std::lock_guard l(p.mutex);
    return p.accepted == p.revision && p.unloaded;
}
VideoSnapshot FFmpegVideo::getSnapshot() const {
    auto &p = *impl_;
    std::lock_guard l(p.mutex);
    return {p.target, p.ready ? p.target : VideoState::None, p.ready, p.error, p.finished, p.videoStream};
}
VideoState FFmpegVideo::getTargetState() const { return getSnapshot().targetState; }
VideoState FFmpegVideo::getActualState() const { return getSnapshot().actualState; }
bool FFmpegVideo::isPipelineReady() const { return getSnapshot().pipelineReady; }
bool FFmpegVideo::hasError() const { return getSnapshot().hasError; }
bool FFmpegVideo::hasFinishedLoops() const { return getSnapshot().hasFinishedLoops; }
bool FFmpegVideo::hasVideoStream() const { return getSnapshot().hasVideoStream; }
SDL_Texture *FFmpegVideo::getTexture() const {
#ifdef RETROFE_HAVE_D3D12
    if (impl_->nativePending && impl_->interop) {
        auto* texture = impl_->interop->currentTexture();
        if (texture) SDL_SetTextureBlendMode(texture, impl_->soft ? softBlend : SDL_BLENDMODE_BLEND);
        return texture;
    }
    if (impl_->interop && impl_->valid && impl_->gpuTexture && impl_->texture == impl_->gpuTexture)
        return impl_->interop->currentTexture();
#endif
#ifdef _WIN32
    if (impl_->interop11 && impl_->texture == impl_->gpuTexture && !impl_->interop11->available()) return nullptr;
#endif
    return impl_->valid ? impl_->texture : nullptr;
}
bool FFmpegVideo::usingGpuTexture() const {
#ifdef RETROFE_HAVE_D3D12
    if (impl_->nativePending && impl_->interop) return impl_->interop->currentTexture() != nullptr;
#endif
    return impl_->valid && impl_->texture && impl_->texture == impl_->gpuTexture;
}
uint64_t FFmpegVideo::gpuFrameCount() const { return impl_->gpuFrames; }
VideoDim FFmpegVideo::getDimensions() { return impl_->dim; }
void FFmpegVideo::setSoftOverlay(bool v) {
    auto &p = *impl_;
    p.soft = v;
    if (p.texture)
        SDL_SetTextureBlendMode(p.texture, p.soft ? softBlend : SDL_BLENDMODE_BLEND);
}
void FFmpegVideo::setVolume(float v) {
    AudioBus::instance().setGain(impl_->audio, Configuration::MuteVideo ? 0 : std::clamp(v, 0.f, 1.f));
}
void FFmpegVideo::setNumLoops(int n) { impl_->loops = std::max(0, n); }
void FFmpegVideo::setPerspectiveCorners(const int *c) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);

    const bool nextPerspective = c != nullptr;
    const bool modeChanged = p.perspective != nextPerspective;
    p.perspective = nextPerspective;

    if (c) {
        for (int i = 0; i < 4; ++i)
            p.corners[i] = {double(c[2 * i]), double(c[2 * i + 1])};
    }

    // Software frames are normalized on the decoder worker. If the output mode
    // changes, discard queued frames prepared for the previous mode.
    if (modeChanged) {
        p.videos.clear();
        p.needsFrame = true;
        p.wake.notify_all();
    }
}
void FFmpegVideo::pause() {
    auto &p = *impl_;
    std::lock_guard l(p.mutex);
    p.position = p.now();
    p.target = VideoState::Paused;
    AudioBus::instance().clear(p.audio);
}
void FFmpegVideo::resume() {
    auto &p = *impl_;
    std::lock_guard l(p.mutex);
    if (p.target != VideoState::Playing) {
        p.anchor = SDL_GetTicksNS();
        p.target = VideoState::Playing;
    }
    p.wake.notify_all();
}
void FFmpegVideo::seek(int64_t t, bool paused) {
    auto &p = *impl_;
    t = std::max<int64_t>(0, t);
    const auto duration = int64_t(getDuration());
    if (duration)
        t = std::min(t, std::max<int64_t>(0, duration - 100000000));
    p.request(p.requested, t, false, paused ? VideoState::Paused : VideoState::Playing);
}
void FFmpegVideo::restart() { seek(0, isPaused()); }
void FFmpegVideo::rewindAndPause() { seek(0, true); }
void FFmpegVideo::loop() {
    auto &p = *impl_;
    p.request(p.requested, 0, false, isPaused() ? VideoState::Paused : VideoState::Playing, true);
}
void FFmpegVideo::skipForward() { seek(int64_t(getCurrent()) + 60000000000, isPaused()); }
void FFmpegVideo::skipBackward() { seek(int64_t(getCurrent()) - 60000000000, isPaused()); }
void FFmpegVideo::skipForwardp() { seek(int64_t(getCurrent() + getDuration() / 20), isPaused()); }
void FFmpegVideo::skipBackwardp() { seek(int64_t(getCurrent()) - int64_t(getDuration() / 20), isPaused()); }
unsigned long long FFmpegVideo::getCurrent() {
    auto &p = *impl_;
    std::lock_guard l(p.mutex);
    return p.ready ? std::max<int64_t>(0, p.duration ? std::min(p.now(), p.duration) : p.now()) : 0;
}
unsigned long long FFmpegVideo::getDuration() {
    std::lock_guard l(impl_->mutex);
    return impl_->duration;
}
bool FFmpegVideo::isPaused() { return getTargetState() == VideoState::Paused; }
bool FFmpegVideo::isPlaying() { return getTargetState() == VideoState::Playing; }
void FFmpegVideo::updateFrame() {
    auto &p = *impl_;
#ifdef RETROFE_HAVE_D3D12
    if (p.nativePending && p.interop) {
        if (auto* submitted = p.interop->currentTexture()) {
            if (p.texture && p.texture != p.gpuTexture) SDL_DestroyTexture(p.texture);
            p.texture = p.gpuTexture = submitted;
            SDL_SetTextureBlendMode(submitted, p.soft ? softBlend : SDL_BLENDMODE_BLEND);
            ++p.gpuFrames;
            std::lock_guard lock(p.mutex);
            p.valid = true;
            p.needsFrame = false;
            p.nativePending = false;
            p.nativePendingSinceNs = 0;
        } else {
            constexpr Uint64 pendingTimeoutNs = 250000000ULL;
            if (p.nativePendingSinceNs && SDL_GetTicksNS() - p.nativePendingSinceNs >= pendingTimeoutNs) {
                LOG_WARNING("FFmpegVideo", "D3D12 native frame submission timed out after 250 ms; requesting a new frame");
                p.interop->invalidateFrame();
                p.nativePending = false;
                p.nativePendingSinceNs = 0;
                std::lock_guard lock(p.mutex);
                p.needsFrame = true;
                p.wake.notify_all();
            }
            return;
        }
    }
#endif
    Frame f;
    bool cpuPrepared = false;
    bool framePerspective = false;
    std::array<Point2D, 4> frameCorners{};
#ifdef RETROFE_HAVE_EGL_DMABUF
    Frame mapped;
#endif
    bool repeat = false;
    {
        std::lock_guard lock(p.mutex);
        if (!p.ready || p.error || p.unloaded)
            return;
        auto time = p.now();
        while (!p.videos.empty() && ((p.needsFrame && !f) || p.videos.front().pts <= time)) {
            f = std::move(p.videos.front().data);
            cpuPrepared = p.videos.front().cpuPrepared;
            framePerspective = p.videos.front().perspective;
#ifdef RETROFE_HAVE_EGL_DMABUF
            mapped = std::move(p.videos.front().drm);
#endif
            p.videos.pop_front();
            if (p.target != VideoState::Playing)
                break;
        }
        frameCorners = p.corners;
        if (p.eof && p.videos.empty() && p.audios.empty() && p.target == VideoState::Playing &&
            (!p.duration || time >= p.duration)) {
            repeat = !p.loops || ++p.playCount < p.loops;
            if (!repeat) {
                p.finished = true;
                p.position = p.duration;
                p.target = VideoState::Paused;
            }
        }
        p.wake.notify_all();
    }
    if (f) {
        SDL_Colorspace color =
            f->color_range == AVCOL_RANGE_JPEG ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
        if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M)
            color =
                f->color_range == AVCOL_RANGE_JPEG ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
        if (f->colorspace == AVCOL_SPC_BT2020_NCL)
            color = f->color_range == AVCOL_RANGE_JPEG ? SDL_COLORSPACE_BT2020_FULL
                                                       : SDL_COLORSPACE_BT2020_LIMITED;
#ifdef RETROFE_HAVE_D3D12
        if (f->format == AV_PIX_FMT_D3D12 && p.interop && !framePerspective) {
            if (!p.interop->available()) {
                if (!p.fallbackRequested.exchange(true)) {
                    LOG_WARNING("FFmpegVideo", "D3D12 interop unavailable (" + std::string(p.interop->reason()) + "); requesting software decoding fallback");
                    p.wake.notify_all();
                }
                return;
            }
            auto *d = reinterpret_cast<AVD3D12VAFrame *>(f->data[0]);
            if (!d || !d->texture || !d->sync_ctx.fence || d->subresource_index < 0) {
                Logger::write(Logger::ZONE_ERROR,"FFmpegVideo","Invalid D3D12 frame descriptor: "+p.requested);
                std::lock_guard lock(p.mutex);
                p.error = true;
                p.valid = false;
                return;
            }
            auto desc = d->texture->GetDesc();
            if (ffmpegDiagnostics() && !p.nativeDescriptionLogged) {
                std::ostringstream details;
                details << p.requested << "; D3D12 frame: format=" << unsigned(desc.Format)
                    << " (NV12=" << unsigned(DXGI_FORMAT_NV12) << "), allocation=" << desc.Width << "x" << desc.Height
                    << "; visible=" << f->width << "x" << f->height << "; array=" << desc.DepthOrArraySize
                    << "; mips=" << desc.MipLevels << "; slice=" << d->subresource_index
                    << "; resourceFlags=0x" << std::hex << unsigned(desc.Flags) << std::dec
                    << "; frameFlags=" << unsigned(d->flags) << "; producerFence=" << d->sync_ctx.fence_value
                    << "; completed=" << d->sync_ctx.fence->GetCompletedValue()
                    << "; resource=" << d->texture;
                Logger::write(Logger::ZONE_INFO,"FFmpegVideo",details.str());
                p.nativeDescriptionLogged = true;
            }
            unsigned y = unsigned(d->subresource_index) * desc.MipLevels;
            const int cropLeft = static_cast<int>(f->crop_left) & ~1;
            const int cropTop = static_cast<int>(f->crop_top) & ~1;
            const int cropRight = static_cast<int>(f->crop_right) & ~1;
            const int cropBottom = static_cast<int>(f->crop_bottom) & ~1;
            const bool hasCrop = (cropLeft > 0 || cropTop > 0 || cropRight > 0 || cropBottom > 0);
            const int visibleW = ((hasCrop ? (f->width - cropLeft - cropRight) : f->width) + 1) & ~1;
            const int visibleH = ((hasCrop ? (f->height - cropTop - cropBottom) : f->height) + 1) & ~1;
            const int cropX = hasCrop ? cropLeft : 0;
            const int cropY = hasCrop ? cropTop : 0;
            const int cropW = hasCrop ? visibleW : 0;
            const int cropH = hasCrop ? visibleH : 0;

            if (auto *texture = p.interop->copyNative(d->texture, d->sync_ctx.fence, d->sync_ctx.fence_value,
                                                      y, y + unsigned(desc.MipLevels) * desc.DepthOrArraySize,
                                                      f->width, f->height, color, f,
                                                      cropX, cropY, cropW, cropH)) {
                if (p.texture && p.texture != p.gpuTexture)
                    SDL_DestroyTexture(p.texture);
                p.texture = p.gpuTexture = texture;
                ++p.gpuFrames;
                p.dim = {visibleW, visibleH};
                SDL_SetTextureBlendMode(texture, p.soft ? softBlend : SDL_BLENDMODE_BLEND);
                {
                    std::lock_guard lock(p.mutex);
                    p.valid = true; p.needsFrame = false;
                }
                if (!p.logged) {
                    LOG_INFO("FFmpegVideo",
                             "Playback ACTIVE: D3D12 hardware decode / NV12 GPU copy, no CPU pixel transfer");
                    p.logged = true;
                }
                if (repeat)
                    p.request(p.requested, 0, false, VideoState::Playing, true);
                return;
            }
            if (p.interop->deferred()) {
                if (!p.interop->currentTexture()) {
                    p.nativePending = true;
                    p.nativePendingSinceNs = SDL_GetTicksNS();
                    p.dim = {visibleW, visibleH};
                    std::lock_guard lock(p.mutex);
                    p.needsFrame = false;
                }
                if (repeat) p.request(p.requested, 0, false, VideoState::Playing, true);
                return;
            }
            if (!p.interop->available()) {
                if (!p.fallbackRequested.exchange(true)) {
                    LOG_WARNING("FFmpegVideo", "D3D12 native copy unavailable (" + std::string(p.interop->reason()) + "); requesting software decoding fallback");
                    p.wake.notify_all();
                }
                return;
            }
            if (!p.logged)
                LOG_WARNING("FFmpegVideo", std::string("Native copy unavailable: ") + p.interop->reason());
        }
#endif
#ifdef _WIN32
        if (f->format == AV_PIX_FMT_D3D11 && p.interop11 && !framePerspective) {
            auto *tex = reinterpret_cast<ID3D11Texture2D *>(f->data[0]);
            if (!tex) {
                Logger::write(Logger::ZONE_ERROR, "FFmpegVideo", "Invalid D3D11 frame descriptor: " + p.requested);
                std::lock_guard lock(p.mutex);
                p.error = true;
                p.valid = false;
                return;
            }
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            unsigned int subresource = static_cast<unsigned int>(reinterpret_cast<intptr_t>(f->data[1]));
            if (auto *texture = p.interop11->copyNative(tex, subresource, desc, color)) {
                if (p.texture && p.texture != p.gpuTexture)
                    SDL_DestroyTexture(p.texture);
                p.texture = p.gpuTexture = texture;
                ++p.gpuFrames;
                const int cropW = f->width - static_cast<int>(f->crop_left) - static_cast<int>(f->crop_right);
                const int cropH = f->height - static_cast<int>(f->crop_top) - static_cast<int>(f->crop_bottom);
                p.dim = {(cropW > 0 ? cropW : f->width), (cropH > 0 ? cropH : f->height)};
                SDL_SetTextureBlendMode(texture, p.soft ? softBlend : SDL_BLENDMODE_BLEND);
                {
                    std::lock_guard lock(p.mutex);
                    p.valid = true;
                    p.needsFrame = false;
                }
                if (!p.logged) {
                    LOG_INFO("FFmpegVideo",
                             "Playback ACTIVE: D3D11 hardware decode / NV12 GPU copy, no CPU pixel transfer");
                    p.logged = true;
                }
                if (repeat)
                    p.request(p.requested, 0, false, VideoState::Playing, true);
                return;
            }
            if (!p.logged)
                LOG_WARNING("FFmpegVideo", std::string("D3D11 native copy unavailable: ") + p.interop11->reason());
        }
#endif
#ifdef RETROFE_HAVE_EGL_DMABUF
        if (p.egl && mapped && !framePerspective) {
            try {
                auto native = ffmpegDmaBufFrame(f, mapped);
                auto* texture = p.egl->copy(native);
                if (!texture) throw std::runtime_error(p.egl->reason());
                if (p.texture && p.texture != p.gpuTexture) SDL_DestroyTexture(p.texture);
                p.texture = p.gpuTexture = texture;
                ++p.gpuFrames;
                p.dim = {p.egl->width(), p.egl->height()};
                SDL_SetTextureBlendMode(texture, p.soft ? softBlend : SDL_BLENDMODE_BLEND);
                { std::lock_guard lock(p.mutex); p.valid = true; p.needsFrame = false; }
                if (!p.logged) {
                    LOG_INFO("FFmpegVideo", std::string("Playback ACTIVE: VAAPI / ") + p.egl->description() + "; no CPU pixel transfer");
                    p.logged = true;
                }
                if (repeat) p.request(p.requested, 0, false, VideoState::Playing, true);
                return;
            } catch (const std::exception& e) {
                if (!p.logged) LOG_WARNING("FFmpegVideo", std::string("EGL import unavailable; CPU transfer fallback: ") + e.what());
            }
        } else if (p.egl && p.exportDmaBuf && !mapped && !p.logged && f->format == AV_PIX_FMT_VAAPI) {
            LOG_WARNING("FFmpegVideo", "VAAPI DMA-BUF export unavailable; CPU transfer fallback");
        }
#endif
        const bool downloaded = f->hw_frames_ctx != nullptr;
        if (f->hw_frames_ctx) {
            auto cpu = frame();
            if (av_hwframe_transfer_data(cpu.get(), f.get(), 0) < 0) {
                LOG_ERROR("FFmpegVideo", "Hardware frame download failed");
                return;
            }
            av_frame_copy_props(cpu.get(), f.get());
            f = cpu;
            cpuPrepared = false;
        }

        if (!cpuPrepared) {
            if (av_frame_apply_cropping(f.get(), 0) < 0) {
                std::lock_guard lock(p.mutex);
                p.error = true;
                p.valid = false;
                LOG_ERROR("FFmpegVideo", "Invalid decoded crop metadata");
                return;
            }

            const auto pixelFormat =
                framePerspective ? AV_PIX_FMT_RGBA : AV_PIX_FMT_YUV420P;
            if (f->format != pixelFormat) {
                Frame converted = frame();
                if (!converted)
                    return;
                converted->format = pixelFormat;
                converted->width = f->width;
                converted->height = f->height;
                if (av_frame_get_buffer(converted.get(), 32) < 0)
                    return;

                p.fallbackScaler = sws_getCachedContext(
                    p.fallbackScaler,
                    f->width, f->height, AVPixelFormat(f->format),
                    f->width, f->height, pixelFormat,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!p.fallbackScaler)
                    return;

                const int matrix =
                    f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
                    f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 :
                    (f->colorspace == AVCOL_SPC_BT470BG ||
                     f->colorspace == AVCOL_SPC_SMPTE170M) ? SWS_CS_ITU601 :
                    SWS_CS_ITU709;
                const int *coefficients = sws_getCoefficients(matrix);
                const int range = f->color_range == AVCOL_RANGE_JPEG;
                sws_setColorspaceDetails(
                    p.fallbackScaler,
                    coefficients, range,
                    coefficients, framePerspective ? 1 : range,
                    0, 1 << 16, 1 << 16);

                if (sws_scale(
                        p.fallbackScaler,
                        f->data, f->linesize,
                        0, f->height,
                        converted->data, converted->linesize) != f->height)
                    return;

                av_frame_copy_props(converted.get(), f.get());
                f = converted;
            }
        }

        if (p.texture == p.gpuTexture && p.gpuTexture) {
            p.texture = nullptr;
            p.gpuTexture = nullptr;
        }

        const auto outputFormat =
            framePerspective ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_IYUV;
        if (framePerspective)
            color = SDL_COLORSPACE_SRGB;

        if (!p.texture || p.textureFormat != outputFormat || p.textureW != f->width ||
            p.textureH != f->height || p.textureColor != color) {
            if (p.texture)
                SDL_DestroyTexture(p.texture);
            p.texture = nullptr;

            auto props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, outputFormat);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, color);
            p.texture = SDL_CreateTextureWithProperties(SDL::getRenderer(p.monitor), props);
            SDL_DestroyProperties(props);

            if (!p.texture) {
                std::lock_guard lock(p.mutex);
                p.error = true;
                p.valid = false;
                LOG_ERROR("FFmpegVideo", std::string("Texture allocation failed: ") + SDL_GetError());
                return;
            }

            SDL_SetTextureBlendMode(
                p.texture,
                p.soft ? softBlend : SDL_BLENDMODE_BLEND);

            p.textureW = f->width;
            p.textureH = f->height;
            p.textureColor = color;
            p.textureFormat = outputFormat;
        }

        bool updated = false;
        if (framePerspective && p.texture) {
            if (cpuPrepared) {
                // Software perspective frames are already converted and warped
                // on the decoder worker.
                updated = SDL_UpdateTexture(
                    p.texture,
                    nullptr,
                    f->data[0],
                    f->linesize[0]);
            } else {
                // Rare hardware-download fallback: retain the old render-thread
                // implementation rather than perturbing native interop behavior.
                auto h = computePerspectiveMatrixFromCorners(f->width, f->height, frameCorners);
                p.warped.assign(size_t(f->width) * f->height * 4, 0);
                for (int y = 0; y < f->height; ++y)
                    for (int x = 0; x < f->width; ++x) {
                        double z = h[6] * x + h[7] * y + h[8];
                        if (std::abs(z) < 1e-9)
                            continue;
                        double sx = (h[0] * x + h[1] * y + h[2]) / z;
                        double sy = (h[3] * x + h[4] * y + h[5]) / z;
                        if (sx < 0 || sy < 0 || sx >= f->width || sy >= f->height)
                            continue;
                        int ix = int(sx), iy = int(sy);
                        int jx = std::min(ix + 1, f->width - 1);
                        int jy = std::min(iy + 1, f->height - 1);
                        double dx = sx - ix, dy = sy - iy;
                        for (int c = 0; c < 4; ++c)
                            p.warped[(size_t(y) * f->width + x) * 4 + c] =
                                uint8_t((1 - dy) *
                                        ((1 - dx) * f->data[0][iy * f->linesize[0] + ix * 4 + c] +
                                         dx * f->data[0][iy * f->linesize[0] + jx * 4 + c]) +
                                        dy *
                                        ((1 - dx) * f->data[0][jy * f->linesize[0] + ix * 4 + c] +
                                         dx * f->data[0][jy * f->linesize[0] + jx * 4 + c]));
                    }
                updated = SDL_UpdateTexture(
                    p.texture,
                    nullptr,
                    p.warped.data(),
                    f->width * 4);
            }
        } else if (p.texture) {
            updated = SDL_UpdateYUVTexture(
                p.texture,
                nullptr,
                f->data[0], f->linesize[0],
                f->data[1], f->linesize[1],
                f->data[2], f->linesize[2]);
        }
        if (updated) {
            p.dim = {f->width, f->height};
            std::lock_guard lock(p.mutex);
            p.valid = true; p.needsFrame = false;
            if (!p.logged) {
                LOG_INFO("FFmpegVideo", std::string("Playback ACTIVE: ") +
                                            (downloaded ? "hardware decode / CPU download and texture upload"
                                                        : "software decode / CPU texture upload"));
                p.logged = true;
            }
        }
    }
    if (repeat)
        p.request(p.requested, 0, false, VideoState::Playing, true);
}
