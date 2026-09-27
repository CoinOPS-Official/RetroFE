// Standalone Windows file-open/first-frame/retirement comparison for RetroFE.
#include <SDL3/SDL.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/gst.h>
#include <gst/video/video.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

std::string hexHr(HRESULT hr) {
    char out[16]{};
    std::snprintf(out, sizeof(out), "0x%08lX", static_cast<unsigned long>(hr));
    return out;
}

void checkHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed: " + hexHr(hr));
}

void checkAv(int result, const char* what) {
    if (result >= 0) return;
    char out[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, out, sizeof(out));
    throw std::runtime_error(std::string(what) + " failed: " + out);
}

std::string utf8FromWide(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Could not convert command line to UTF-8");
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                             result.data(), count, nullptr, nullptr))
        throw std::runtime_error("Could not convert command line to UTF-8");
    return result;
}

std::wstring widePath(const std::string& utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (length <= 0) throw std::runtime_error("Path is not valid UTF-8");
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, wide.data(), length))
        throw std::runtime_error("Could not convert media path to UTF-16");
    wide.pop_back();
    return wide;
}

double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct Frame {
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<void> owner;
    UINT subresource = 0;
    int visibleWidth = 0;
    int visibleHeight = 0;
    std::string path;
};

class Mailbox {
public:
    void publish(Frame frame) {
        std::lock_guard lock(mutex_);
        if (!firstDecoded_) firstDecoded_ = Clock::now();
        latest_ = std::move(frame); // RetroFE-style newest-frame staging
    }
    std::optional<Frame> take() {
        std::lock_guard lock(mutex_);
        auto result = std::move(latest_);
        latest_.reset();
        return result;
    }
    void fail(std::string message) {
        std::lock_guard lock(mutex_);
        if (error_.empty()) error_ = std::move(message);
    }
    void finished() { done_.store(true); }
    std::optional<Clock::time_point> firstDecoded() const {
        std::lock_guard lock(mutex_);
        return firstDecoded_;
    }
    std::string error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }
    bool done() const { return done_.load(); }
private:
    mutable std::mutex mutex_;
    std::optional<Frame> latest_;
    std::optional<Clock::time_point> firstDecoded_;
    std::string error_;
    std::atomic<bool> done_{false};
};

class Presenter {
public:
    Presenter(SDL_Renderer* renderer, ID3D11Device* device) : renderer_(renderer), device_(device) {
        device_->GetImmediateContext(context_.GetAddressOf());
        if (!context_) throw std::runtime_error("SDL renderer has no D3D11 immediate context");
        if (SUCCEEDED(context_.As(&multithread_)) && multithread_)
            multithread_->SetMultithreadProtected(TRUE);
        else throw std::runtime_error("SDL D3D11 renderer lacks thread protection");
    }
    ~Presenter() {
        SDL_FlushRenderer(renderer_);
        for (auto& slot : slots_) if (slot.sdl) SDL_DestroyTexture(slot.sdl);
    }

    bool present(const Frame& frame) {
        if (!frame.texture || frame.visibleWidth <= 0 || frame.visibleHeight <= 0) return false;
        ComPtr<ID3D11Device> sourceDevice;
        frame.texture->GetDevice(sourceDevice.GetAddressOf());
        if (sourceDevice.Get() != device_.Get()) throw std::runtime_error("Decoder output uses a different D3D11 device");
        D3D11_TEXTURE2D_DESC source{};
        frame.texture->GetDesc(&source);
        if (source.Format != DXGI_FORMAT_NV12 || source.Width < static_cast<UINT>(frame.visibleWidth) ||
            source.Height < static_cast<UINT>(frame.visibleHeight))
            throw std::runtime_error("Decoder output is not a valid NV12 texture");
        if (source.Width != width_ || source.Height != height_) makeSlots(source.Width, source.Height);
        Slot& slot = slots_[next_++ % slots_.size()];
        if (!SDL_FlushRenderer(renderer_))
            throw std::runtime_error(std::string("SDL_FlushRenderer: ") + SDL_GetError());
        context_->CopySubresourceRegion(slot.native, 0, 0, 0, 0,
                                        frame.texture.Get(), frame.subresource, nullptr);

        if (!SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) || !SDL_RenderClear(renderer_))
            throw std::runtime_error(std::string("SDL_RenderClear: ") + SDL_GetError());
        int outW = 0, outH = 0;
        if (!SDL_GetRenderOutputSize(renderer_, &outW, &outH))
            throw std::runtime_error(std::string("SDL_GetRenderOutputSize: ") + SDL_GetError());
        const float scale = std::min(static_cast<float>(outW) / frame.visibleWidth,
                                     static_cast<float>(outH) / frame.visibleHeight);
        const SDL_FRect src{0, 0, static_cast<float>(frame.visibleWidth), static_cast<float>(frame.visibleHeight)};
        const SDL_FRect dst{(outW - frame.visibleWidth * scale) * .5f,
                            (outH - frame.visibleHeight * scale) * .5f,
                            frame.visibleWidth * scale, frame.visibleHeight * scale};
        if (!SDL_RenderTexture(renderer_, slot.sdl, &src, &dst))
            throw std::runtime_error(std::string("SDL_RenderTexture: ") + SDL_GetError());
        if (!SDL_RenderPresent(renderer_))
            throw std::runtime_error(std::string("SDL present: ") + SDL_GetError());
        return true;
    }

    void black() {
        if (!SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) || !SDL_RenderClear(renderer_) ||
            !SDL_RenderPresent(renderer_))
            throw std::runtime_error(std::string("SDL black frame: ") + SDL_GetError());
    }
private:
    struct Slot { SDL_Texture* sdl = nullptr; ID3D11Texture2D* native = nullptr; };
    void makeSlots(UINT width, UINT height) {
        if (!SDL_FlushRenderer(renderer_))
            throw std::runtime_error(std::string("SDL_FlushRenderer before resize: ") + SDL_GetError());
        for (auto& slot : slots_) { if (slot.sdl) SDL_DestroyTexture(slot.sdl); slot = {}; }
        width_ = width; height_ = height; next_ = 0;
        for (auto& slot : slots_) {
            slot.sdl = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STATIC,
                                         static_cast<int>(width), static_cast<int>(height));
            if (!slot.sdl) throw std::runtime_error(std::string("SDL_CreateTexture(NV12): ") + SDL_GetError());
            slot.native = static_cast<ID3D11Texture2D*>(SDL_GetPointerProperty(
                SDL_GetTextureProperties(slot.sdl), SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
            if (!slot.native) throw std::runtime_error("SDL NV12 texture has no D3D11 resource");
            SDL_SetTextureScaleMode(slot.sdl, SDL_SCALEMODE_LINEAR);
        }
    }
    SDL_Renderer* renderer_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D10Multithread> multithread_;
    std::array<Slot, 3> slots_{};
    UINT width_ = 0, height_ = 0;
    size_t next_ = 0;
};

struct GStreamerInit {
    GStreamerInit() { gst_init(nullptr, nullptr); }
    ~GStreamerInit() { gst_deinit(); }
};

void preferGstD3D11() {
    GstRegistry* registry = gst_registry_get();
    for (const char* codec : {"h264", "h265", "vp9", "mpeg2", "av1"}) {
        for (const auto [prefix, rank] : std::array<std::pair<const char*, int>, 2>{
                 std::pair{"d3d11", static_cast<int>(GST_RANK_PRIMARY) + 100},
                 std::pair{"d3d12", static_cast<int>(GST_RANK_NONE)}}) {
            const std::string name = std::string(prefix) + codec + "dec";
            if (auto* feature = gst_registry_lookup_feature(registry, name.c_str())) {
                gst_plugin_feature_set_rank(feature, rank);
                gst_object_unref(feature);
            }
        }
    }
}

// Source Reader owns its inserted decoder, so it cannot hand that decoder to
// a reader for another URL. Keep the decoder MFT here and use a Source Reader
// only to demux compressed H.264 samples from each file.
class MediaFoundationMftRetained {
public:
    MediaFoundationMftRetained(IMFDXGIDeviceManager* manager, std::mutex& graphicsMutex)
        : manager_(manager), graphicsMutex_(graphicsMutex) {}

    void run(const std::string& file, Mailbox& mailbox, std::stop_token stop) {
        constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        constexpr DWORD allStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
        checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "MF MFT CoInitializeEx");
        struct CoExit { ~CoExit() { CoUninitialize(); } } coExit;
        ComPtr<IMFSourceReader> source;
        const auto path = widePath(file);
        checkHr(MFCreateSourceReaderFromURL(path.c_str(), nullptr, source.GetAddressOf()),
                "MF compressed Source Reader");
        checkHr(source->SetStreamSelection(allStreams, FALSE), "MF disable unused streams");
        checkHr(source->SetStreamSelection(videoStream, TRUE), "MF enable compressed video");
        ComPtr<IMFMediaType> input;
        checkHr(source->GetCurrentMediaType(videoStream, input.GetAddressOf()), "MF compressed media type");
        GUID subtype{};
        checkHr(input->GetGUID(MF_MT_SUBTYPE, &subtype), "MF compressed subtype");
        if (!IsEqualGUID(subtype, MFVideoFormat_H264))
            throw std::runtime_error("MF retained MFT currently supports H.264 files only");
        UINT32 width = 0, height = 0;
        checkHr(MFGetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, &width, &height), "MF compressed frame size");
        std::vector<uint8_t> header = sequenceHeader(input.Get());
        if (header.empty())
            throw std::runtime_error("MF compressed media type has no H.264 sequence header for safe reuse");

        const bool reused = decoder_ && IsEqualGUID(subtype, subtype_) && width == width_ &&
                            height == height_ && header == sequenceHeader_;
        if (reused) {
            checkHr(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "MF decoder flush for new file");
        } else {
            decoder_.Reset();
            configureDecoder(input.Get(), subtype);
            subtype_ = subtype;
            width_ = width;
            height_ = height;
            sequenceHeader_ = std::move(header);
        }
        checkHr(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MF begin streaming");
        checkHr(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MF start of stream");
        const std::string transition = reused ? "reused decoder MFT; new compressed Source Reader" :
                                                "created decoder MFT; new compressed Source Reader";

        auto pumpOutput = [&]() -> bool {
            bool produced = false;
            for (int guard = 0; guard < 32 && !stop.stop_requested(); ++guard) {
                MFT_OUTPUT_DATA_BUFFER output{};
                output.dwStreamID = outputId_;
                DWORD status = 0;
                HRESULT hr;
                {
                    std::lock_guard lock(graphicsMutex_);
                    hr = decoder_->ProcessOutput(0, 1, &output, &status);
                }
                ComPtr<IMFSample> decoded;
                decoded.Attach(output.pSample);
                if (output.pEvents) output.pEvents->Release();
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return produced;
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                    setNv12Output();
                    continue;
                }
                checkHr(hr, "MF decoder ProcessOutput");
                if (decoded) {
                    publishDecoded(decoded.Detach(), width_, height_, transition, mailbox);
                    produced = true;
                }
            }
            return produced;
        };

        while (!stop.stop_requested()) {
            DWORD stream = 0, flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> compressed;
            checkHr(source->ReadSample(videoStream, 0, &stream, &flags, &timestamp,
                                       compressed.GetAddressOf()), "MF compressed ReadSample");
            if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("MF compressed reader error");
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                throw std::runtime_error("MF compressed media type changed within a file");
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!compressed) continue;
            HRESULT hr;
            {
                std::lock_guard lock(graphicsMutex_);
                hr = decoder_->ProcessInput(inputId_, compressed.Get(), 0);
            }
            if (hr == MF_E_NOTACCEPTING) {
                if (!pumpOutput()) throw std::runtime_error("MF decoder rejected input without output");
                {
                    std::lock_guard lock(graphicsMutex_);
                    hr = decoder_->ProcessInput(inputId_, compressed.Get(), 0);
                }
            }
            checkHr(hr, "MF decoder ProcessInput");
            if (pumpOutput()) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

private:
    static std::vector<uint8_t> sequenceHeader(IMFMediaType* type) {
        UINT32 count = 0;
        if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &count)) || !count) return {};
        std::vector<uint8_t> bytes(count);
        checkHr(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, bytes.data(), count, &count),
                "MF H.264 sequence header");
        bytes.resize(count);
        return bytes;
    }

    void configureDecoder(IMFMediaType* input, const GUID& subtype) {
        MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, subtype};
        MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_NV12};
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        checkHr(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                          &inputInfo, &outputInfo, &activations, &count), "MFTEnumEx video decoder");
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFTransform> candidate;
            if (SUCCEEDED(activations[i]->ActivateObject(IID_PPV_ARGS(candidate.GetAddressOf())))) {
                ComPtr<IMFAttributes> attributes;
                UINT32 aware = 0;
                if (SUCCEEDED(candidate->GetAttributes(attributes.GetAddressOf())) &&
                    SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware &&
                    SUCCEEDED(candidate->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                       reinterpret_cast<ULONG_PTR>(manager_.Get()))) &&
                    SUCCEEDED(candidate->SetInputType(0, input, 0))) {
                    decoder_ = std::move(candidate);
                }
            }
            activations[i]->Release();
            if (decoder_) {
                for (++i; i < count; ++i) activations[i]->Release();
                break;
            }
        }
        CoTaskMemFree(activations);
        if (!decoder_) throw std::runtime_error("No synchronous D3D11-aware MF H.264 decoder MFT accepted the file");
        if (FAILED(decoder_->GetStreamIDs(1, &inputId_, 1, &outputId_))) {
            inputId_ = 0; outputId_ = 0;
        }
        setNv12Output();
    }

    void setNv12Output() {
        bool selected = false;
        for (DWORD index = 0; index < 32; ++index) {
            ComPtr<IMFMediaType> type;
            if (FAILED(decoder_->GetOutputAvailableType(outputId_, index, type.GetAddressOf()))) break;
            GUID subtype{};
            if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && IsEqualGUID(subtype, MFVideoFormat_NV12) &&
                SUCCEEDED(decoder_->SetOutputType(outputId_, type.Get(), 0))) {
                selected = true;
                break;
            }
        }
        if (!selected) throw std::runtime_error("MF decoder MFT offered no NV12 output type");
        MFT_OUTPUT_STREAM_INFO info{};
        checkHr(decoder_->GetOutputStreamInfo(outputId_, &info), "MF decoder output stream info");
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
            throw std::runtime_error("MF decoder MFT requires caller-owned output samples");
    }

    static void publishDecoded(IMFSample* raw, UINT32 width, UINT32 height,
                               const std::string& transition, Mailbox& mailbox) {
        ComPtr<IMFSample> sample;
        sample.Attach(raw);
        ComPtr<IMFMediaBuffer> buffer;
        checkHr(sample->GetBufferByIndex(0, buffer.GetAddressOf()), "MF MFT output buffer");
        ComPtr<IMFDXGIBuffer> dxgi;
        checkHr(buffer.As(&dxgi), "MF retained MFT output is not DXGI");
        ComPtr<ID3D11Texture2D> texture;
        checkHr(dxgi->GetResource(IID_PPV_ARGS(texture.GetAddressOf())), "MF retained MFT DXGI resource");
        UINT subresource = 0;
        checkHr(dxgi->GetSubresourceIndex(&subresource), "MF retained MFT subresource");
        Frame frame;
        frame.texture = std::move(texture);
        frame.subresource = subresource;
        frame.visibleWidth = static_cast<int>(width);
        frame.visibleHeight = static_cast<int>(height);
        frame.path = "MF D3D11 NV12; " + transition;
        frame.owner = std::shared_ptr<IMFSample>(sample.Detach(), [](IMFSample* value) { value->Release(); });
        mailbox.publish(std::move(frame));
    }

    ComPtr<IMFDXGIDeviceManager> manager_;
    std::mutex& graphicsMutex_;
    ComPtr<IMFTransform> decoder_;
    GUID subtype_{};
    UINT32 width_ = 0, height_ = 0;
    std::vector<uint8_t> sequenceHeader_;
    DWORD inputId_ = 0, outputId_ = 0;
};

AVPixelFormat chooseD3D11(AVCodecContext*, const AVPixelFormat* formats) {
    for (auto* choice = formats; *choice != AV_PIX_FMT_NONE; ++choice)
        if (*choice == AV_PIX_FMT_D3D11) return *choice;
    return AV_PIX_FMT_NONE;
}

class FFmpegRetained {
public:
    explicit FFmpegRetained(ID3D11Device* device, std::mutex& graphicsMutex) : graphicsMutex_(graphicsMutex) {
        hardware_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hardware_) throw std::runtime_error("av_hwdevice_ctx_alloc(D3D11VA) failed");
        auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hardware_->data)->hwctx);
        hw->device = device;
        device->AddRef();
        try { checkAv(av_hwdevice_ctx_init(hardware_), "av_hwdevice_ctx_init"); }
        catch (...) { av_buffer_unref(&hardware_); throw; }
    }
    ~FFmpegRetained() {
        if (format_) avformat_close_input(&format_);
        avcodec_free_context(&decoder_);
        avcodec_parameters_free(&parameters_);
        av_buffer_unref(&hardware_);
    }
    FFmpegRetained(const FFmpegRetained&) = delete;
    FFmpegRetained& operator=(const FFmpegRetained&) = delete;

    std::string open(const std::string& file, std::stop_token stop) {
        stop_ = stop;
        if (format_ && file == loaded_) {
            checkAv(av_seek_frame(format_, stream_, 0, AVSEEK_FLAG_BACKWARD), "same-file seek");
            avcodec_flush_buffers(decoder_);
            return "same-file seek, retained demuxer and decoder";
        }
        if (format_) avformat_close_input(&format_);
        loaded_.clear();
        format_ = avformat_alloc_context();
        if (!format_) throw std::bad_alloc();
        format_->interrupt_callback = {[](void* p) -> int {
            return static_cast<FFmpegRetained*>(p)->stop_.stop_requested() ? 1 : 0;
        }, this};
        checkAv(avformat_open_input(&format_, file.c_str(), nullptr, nullptr), "avformat_open_input");
        checkAv(avformat_find_stream_info(format_, nullptr), "avformat_find_stream_info");
        stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        checkAv(stream_, "av_find_best_stream video");
        AVStream* video = format_->streams[stream_];
        const AVCodecParameters* next = video->codecpar;
        const bool reuse = compatible(next);
        if (reuse) {
            avcodec_flush_buffers(decoder_);
            decoder_->pkt_timebase = video->time_base;
        } else {
            avcodec_free_context(&decoder_);
            avcodec_parameters_free(&parameters_);
            const AVCodec* codec = avcodec_find_decoder(next->codec_id);
            if (!codec) throw std::runtime_error("No FFmpeg video decoder");
            decoder_ = avcodec_alloc_context3(codec);
            if (!decoder_) throw std::bad_alloc();
            checkAv(avcodec_parameters_to_context(decoder_, next), "avcodec_parameters_to_context");
            decoder_->pkt_timebase = video->time_base;
            decoder_->hw_device_ctx = av_buffer_ref(hardware_);
            decoder_->extra_hw_frames = 16;
            decoder_->get_format = &chooseD3D11;
            checkAv(avcodec_open2(decoder_, codec, nullptr), "avcodec_open2");
            parameters_ = avcodec_parameters_alloc();
            if (!parameters_) throw std::bad_alloc();
            checkAv(avcodec_parameters_copy(parameters_, next), "avcodec_parameters_copy");
        }
        loaded_ = file;
        return reuse ? "reused exact-compatible decoder, reopened demuxer" :
                       "created decoder, reopened demuxer";
    }

    void run(const std::string& file, Mailbox& mailbox, std::stop_token stop) {
        const std::string transition = open(file, stop);
        std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(),
            [](AVPacket* p) { av_packet_free(&p); });
        if (!packet) throw std::bad_alloc();
        while (!stop.stop_requested()) {
            int result = av_read_frame(format_, packet.get());
            if (result == AVERROR_EOF) break;
            if (stop.stop_requested()) break;
            checkAv(result, "av_read_frame");
            if (packet->stream_index != stream_) { av_packet_unref(packet.get()); continue; }
            {
                std::lock_guard lock(graphicsMutex_);
                result = avcodec_send_packet(decoder_, packet.get());
            }
            av_packet_unref(packet.get());
            if (result == AVERROR(EAGAIN)) continue;
            checkAv(result, "avcodec_send_packet");
            while (!stop.stop_requested()) {
                AVFrame* raw = av_frame_alloc();
                if (!raw) throw std::bad_alloc();
                std::shared_ptr<AVFrame> decoded(raw, [](AVFrame* p) { av_frame_free(&p); });
                {
                    std::lock_guard lock(graphicsMutex_);
                    result = avcodec_receive_frame(decoder_, decoded.get());
                }
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                checkAv(result, "avcodec_receive_frame");
                if (decoded->format != AV_PIX_FMT_D3D11 || !decoded->data[0])
                    throw std::runtime_error("FFmpeg did not return D3D11 hardware frames");
                Frame frame;
                frame.texture = reinterpret_cast<ID3D11Texture2D*>(decoded->data[0]);
                frame.subresource = static_cast<UINT>(reinterpret_cast<uintptr_t>(decoded->data[1]));
                frame.visibleWidth = decoded->width;
                frame.visibleHeight = decoded->height;
                frame.path = "FFmpeg D3D11VA NV12; " + transition;
                frame.owner = std::move(decoded);
                mailbox.publish(std::move(frame));
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
    }
private:
    std::mutex& graphicsMutex_;
    bool compatible(const AVCodecParameters* next) const {
        if (!decoder_ || !parameters_ || !next) return false;
        const auto& a = *parameters_;
        const auto& b = *next;
        return !a.nb_coded_side_data && !b.nb_coded_side_data &&
            a.codec_type == b.codec_type && a.codec_id == b.codec_id &&
            a.codec_tag == b.codec_tag && a.format == b.format &&
            a.width == b.width && a.height == b.height &&
            a.profile == b.profile && a.level == b.level &&
            a.bits_per_coded_sample == b.bits_per_coded_sample &&
            a.bits_per_raw_sample == b.bits_per_raw_sample &&
            a.field_order == b.field_order && a.video_delay == b.video_delay &&
            a.color_range == b.color_range && a.color_primaries == b.color_primaries &&
            a.color_trc == b.color_trc && a.color_space == b.color_space &&
            a.chroma_location == b.chroma_location &&
            av_cmp_q(a.sample_aspect_ratio, b.sample_aspect_ratio) == 0 &&
            av_cmp_q(a.framerate, b.framerate) == 0 &&
            a.extradata_size > 0 && a.extradata_size == b.extradata_size &&
            a.extradata && b.extradata &&
            std::memcmp(a.extradata, b.extradata, static_cast<size_t>(a.extradata_size)) == 0;
    }
    AVBufferRef* hardware_ = nullptr;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* decoder_ = nullptr;
    AVCodecParameters* parameters_ = nullptr;
    int stream_ = -1;
    std::string loaded_;
    std::stop_token stop_;
};

GstBusSyncReply onNeedContext(GstBus*, GstMessage* message, gpointer user) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_NEED_CONTEXT) {
        const gchar* type = nullptr;
        gst_message_parse_context_type(message, &type);
        if (type && std::string_view(type) == GST_D3D11_DEVICE_HANDLE_CONTEXT_TYPE)
            gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), static_cast<GstContext*>(user));
    }
    return GST_BUS_PASS;
}

class GStreamerRetained {
public:
    GStreamerRetained(ID3D11Device* device, GstD3D11Device* wrapped) : device_(device) {
        gstDevice_ = GST_D3D11_DEVICE(gst_object_ref(wrapped));
        context_ = gst_d3d11_context_new(gstDevice_);
        if (!context_) throw std::runtime_error("gst_d3d11_context_new failed");
        playbin_ = gst_element_factory_make("playbin3", "cycle-player");
        if (!playbin_) throw std::runtime_error("GStreamer playbin3 unavailable");
        sink_ = gst_element_factory_make("appsink", "cycle-sink");
        if (!sink_) throw std::runtime_error("GStreamer appsink unavailable");
        gst_object_ref_sink(sink_); // retain our reference in addition to playbin's
        GstCaps* caps = gst_caps_from_string("video/x-raw(memory:D3D11Memory),format=NV12");
        g_object_set(sink_, "caps", caps, "max-buffers", 1, "drop", TRUE, "sync", TRUE,
                     "enable-last-sample", FALSE, "wait-on-eos", FALSE, nullptr);
        gst_caps_unref(caps);
        constexpr gint videoOnly = 1 << 0;
        g_object_set(playbin_, "flags", videoOnly, "video-sink", sink_, nullptr);
        gst_element_set_context(playbin_, context_);
        gst_element_set_context(sink_, context_);
        bus_ = gst_element_get_bus(playbin_);
        gst_bus_set_sync_handler(bus_, onNeedContext, context_, nullptr);
    }
    ~GStreamerRetained() {
        if (bus_) gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
        if (playbin_) gst_element_set_state(playbin_, GST_STATE_NULL);
        if (bus_) gst_object_unref(bus_);
        if (playbin_) gst_object_unref(playbin_);
        if (sink_) gst_object_unref(sink_);
        if (context_) gst_context_unref(context_);
        if (gstDevice_) gst_object_unref(gstDevice_);
    }
    GStreamerRetained(const GStreamerRetained&) = delete;
    GStreamerRetained& operator=(const GStreamerRetained&) = delete;

    void run(const std::string& file, Mailbox& mailbox, std::stop_token stop) {
        GError* uriError = nullptr;
        gchar* uri = gst_filename_to_uri(file.c_str(), &uriError);
        if (!uri) {
            const std::string text = uriError ? uriError->message : "URI conversion failed";
            g_clear_error(&uriError);
            throw std::runtime_error(text);
        }
        const bool reused = !loaded_.empty();
        // playbin3's instant-uri path is the path used by RetroFE for a
        // retained decoder graph. Drain prior appsink samples while PAUSED.
        if (reused) {
            while (auto* old = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0)) gst_sample_unref(old);
            g_object_set(playbin_, "instant-uri", TRUE, nullptr);
        }
        g_object_set(playbin_, "uri", uri, nullptr);
        if (reused) g_object_set(playbin_, "instant-uri", FALSE, nullptr);
        g_free(uri);
        loaded_ = file;
        if (gst_element_set_state(playbin_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("GStreamer pipeline refused PLAYING");
        while (!stop.stop_requested()) {
            while (GstMessage* message = gst_bus_pop(bus_)) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError* error = nullptr;
                    gchar* debug = nullptr;
                    gst_message_parse_error(message, &error, &debug);
                    const std::string text = error ? error->message : "unknown GStreamer error";
                    g_clear_error(&error); g_free(debug); gst_message_unref(message);
                    throw std::runtime_error(text);
                }
                const bool eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
                gst_message_unref(message);
                if (eos) break;
            }
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 20 * GST_MSECOND);
            if (!sample) continue;
            std::shared_ptr<GstSample> sampleOwner(sample, [](GstSample* p) { gst_sample_unref(p); });
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstCaps* sampleCaps = gst_sample_get_caps(sample);
            GstVideoInfo info{};
            if (!buffer || !sampleCaps || !gst_video_info_from_caps(&info, sampleCaps) ||
                gst_buffer_n_memory(buffer) != 1)
                throw std::runtime_error("GStreamer did not return one NV12 D3D11 sample");
            GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
            if (!memory || !gst_is_d3d11_memory(memory))
                throw std::runtime_error("GStreamer output is not D3D11Memory");
            auto* d3d = GST_D3D11_MEMORY_CAST(memory);
            if (gst_d3d11_device_get_device_handle(d3d->device) != device_.Get())
                throw std::runtime_error("GStreamer decoder did not share SDL's D3D11 device");
            Frame frame;
            frame.texture = static_cast<ID3D11Texture2D*>(gst_d3d11_memory_get_resource_handle(d3d));
            frame.subresource = gst_d3d11_memory_get_subresource_index(d3d);
            frame.visibleWidth = GST_VIDEO_INFO_WIDTH(&info);
            frame.visibleHeight = GST_VIDEO_INFO_HEIGHT(&info);
            frame.path = reused ? "GStreamer D3D11Memory NV12; reused playbin3/instant-uri" :
                                  "GStreamer D3D11Memory NV12; new playbin3";
            frame.owner = std::move(sampleOwner);
            mailbox.publish(std::move(frame));
        }
        // Keep playbin3/decodebin3 and their allocations warm for the next URI.
        if (gst_element_set_state(playbin_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("GStreamer pipeline refused PAUSED");
        gst_element_get_state(playbin_, nullptr, nullptr, 2 * GST_SECOND);
        while (auto* old = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0)) gst_sample_unref(old);
    }
private:
    ComPtr<ID3D11Device> device_;
    GstD3D11Device* gstDevice_ = nullptr;
    GstContext* context_ = nullptr;
    GstElement* playbin_ = nullptr;
    GstElement* sink_ = nullptr;
    GstBus* bus_ = nullptr;
    std::string loaded_;
};

enum class Backend { MediaFoundation, FFmpeg, GStreamer };
const char* backendName(Backend value) {
    switch (value) {
        case Backend::MediaFoundation: return "mf";
        case Backend::FFmpeg: return "ffmpeg";
        case Backend::GStreamer: return "gstreamer";
    }
    return "unknown";
}

struct Options {
    std::vector<std::string> files;
    std::vector<Backend> backends{Backend::MediaFoundation, Backend::FFmpeg, Backend::GStreamer};
    std::vector<bool> reuseModes{false, true};
    int rounds = 5;
    int cycleMs = 500;
    bool hidden = false;
    std::string csv = "cycle-results.csv";
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto value = [&]() -> std::string_view {
            if (++i >= argc) throw std::runtime_error(std::string("Missing value for ") + std::string(arg));
            return argv[i];
        };
        if (arg == "--backend") {
            const auto selected = value();
            if (selected == "mf") options.backends = {Backend::MediaFoundation};
            else if (selected == "ffmpeg") options.backends = {Backend::FFmpeg};
            else if (selected == "gstreamer") options.backends = {Backend::GStreamer};
            else if (selected == "all") options.backends = {Backend::MediaFoundation, Backend::FFmpeg, Backend::GStreamer};
            else throw std::runtime_error("Unknown backend: " + std::string(selected));
        } else if (arg == "--mode") {
            const auto selected = value();
            if (selected == "fresh") options.reuseModes = {false};
            else if (selected == "reuse") options.reuseModes = {true};
            else if (selected == "both") options.reuseModes = {false, true};
            else throw std::runtime_error("Unknown mode: " + std::string(selected));
        } else if (arg == "--rounds") options.rounds = std::stoi(std::string(value()));
        else if (arg == "--cycle-ms") options.cycleMs = std::stoi(std::string(value()));
        else if (arg == "--csv") options.csv = value();
        else if (arg == "--hidden") options.hidden = true;
        else if (arg == "--help") {
            std::cout << "Usage: mediafoundation_cycle [--backend all|mf|ffmpeg|gstreamer] "
                         "[--mode fresh|reuse|both] "
                         "[--rounds N] [--cycle-ms N] [--csv FILE] [--hidden] FILE [FILE...]\n";
            std::exit(0);
        } else if (arg.starts_with("--")) throw std::runtime_error("Unknown option: " + std::string(arg));
        else options.files.emplace_back(arg);
    }
    if (options.files.empty() || options.rounds < 1 || options.cycleMs < 1)
        throw std::runtime_error("Provide one or more files and positive --rounds/--cycle-ms");
    for (auto& file : options.files) {
        const auto path = std::filesystem::absolute(std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(file.data()), file.size())));
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("Media file not found: " + file);
        const auto utf8 = path.u8string();
        file.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
    return options;
}

std::string csvCell(std::string value) {
    std::string escaped = "\"";
    for (char c : value) { if (c == '"') escaped += '"'; escaped += c; }
    escaped += '"';
    return escaped;
}

} // namespace

int wmain(int argc, wchar_t** wideArgv) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) arguments.push_back(utf8FromWide(wideArgv[i]));
        std::vector<char*> argv;
        argv.reserve(arguments.size());
        for (auto& argument : arguments) argv.push_back(argument.data());
        Options options = parseOptions(argc, argv.data());
        checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "main CoInitializeEx");
        struct CoExit { ~CoExit() { CoUninitialize(); } } coExit;
        checkHr(MFStartup(MF_VERSION), "MFStartup");
        struct MfExit { ~MfExit() { MFShutdown(); } } mfExit;
        GStreamerInit gstInit;
        preferGstD3D11();
        // Match RetroFE's D3D11VideoInterop setup before SDL creates its device.
        SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1");
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        struct SdlExit { ~SdlExit() { SDL_Quit(); } } sdlExit;
        SDL_Window* window = SDL_CreateWindow("MF / FFmpeg / GStreamer first-frame cycle", 960, 540,
            options.hidden ? SDL_WINDOW_HIDDEN : 0);
        if (!window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        std::unique_ptr<SDL_Window, void(*)(SDL_Window*)> windowOwner(window, SDL_DestroyWindow);
        SDL_Renderer* renderer = SDL_CreateRenderer(window, "direct3d11");
        if (!renderer) throw std::runtime_error(std::string("SDL_CreateRenderer(direct3d11): ") + SDL_GetError());
        std::unique_ptr<SDL_Renderer, void(*)(SDL_Renderer*)> rendererOwner(renderer, SDL_DestroyRenderer);
        SDL_SetRenderVSync(renderer, 0);
        auto* rawDevice = static_cast<ID3D11Device*>(SDL_GetPointerProperty(
            SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
        if (!rawDevice) throw std::runtime_error("SDL D3D11 renderer exposes no device");
        ComPtr<ID3D11Device> device = rawDevice;
        Presenter presenter(renderer, device.Get());
        GstD3D11Device* gstDevice = gst_d3d11_device_new_wrapped(device.Get());
        if (!gstDevice) throw std::runtime_error("gst_d3d11_device_new_wrapped failed");
        std::unique_ptr<GstD3D11Device, void(*)(GstD3D11Device*)> gstDeviceOwner(
            gstDevice, [](GstD3D11Device* value) { gst_object_unref(value); });
        ComPtr<IMFDXGIDeviceManager> manager;
        UINT resetToken = 0;
        checkHr(MFCreateDXGIDeviceManager(&resetToken, manager.GetAddressOf()), "MFCreateDXGIDeviceManager");
        checkHr(manager->ResetDevice(device.Get(), resetToken), "DXGI manager ResetDevice");

        std::ofstream csv(options.csv, std::ios::trunc);
        if (!csv) throw std::runtime_error("Cannot open CSV: " + options.csv);
        csv << "round,file,backend,mode,decoded_ms,presented_ms,stop_ms,frames_presented,path,error\n";
        std::cout << "SDL " << SDL_GetVersion() << "; " << gst_version_string()
                  << "; FFmpeg " << av_version_info() << "; cycle-ms=" << options.cycleMs << '\n';
        bool quit = false;
        bool hadFailure = false;
        std::map<std::string, std::vector<double>> presentationTimes;
        std::map<std::string, int> misses;
        std::unique_ptr<MediaFoundationMftRetained> warmMediaFoundation;
        std::unique_ptr<FFmpegRetained> warmFFmpeg;
        std::mutex graphicsMutex;
        std::unique_ptr<GStreamerRetained> warmGStreamer;
        for (int round = 1; round <= options.rounds && !quit; ++round) {
            for (const auto& file : options.files) {
                for (Backend backend : options.backends) {
                  for (bool reuse : options.reuseModes) {
                    presenter.black(); // old pixels cannot count as the next first frame
                    Mailbox mailbox;
                    const auto openedAt = Clock::now();
                    std::jthread worker([&, backend, reuse](std::stop_token stop) {
                        try {
                            switch (backend) {
                                case Backend::MediaFoundation:
                                    if (reuse) {
                                        if (!warmMediaFoundation)
                                            warmMediaFoundation = std::make_unique<MediaFoundationMftRetained>(manager.Get(), graphicsMutex);
                                        warmMediaFoundation->run(file, mailbox, stop);
                                    } else {
                                        MediaFoundationMftRetained fresh(manager.Get(), graphicsMutex);
                                        fresh.run(file, mailbox, stop);
                                    }
                                    break;
                                case Backend::FFmpeg: {
                                    if (reuse) {
                                        if (!warmFFmpeg) warmFFmpeg = std::make_unique<FFmpegRetained>(device.Get(), graphicsMutex);
                                        warmFFmpeg->run(file, mailbox, stop);
                                    } else {
                                        FFmpegRetained fresh(device.Get(), graphicsMutex);
                                        fresh.run(file, mailbox, stop);
                                    }
                                    break;
                                }
                                case Backend::GStreamer: {
                                    if (reuse) {
                                        if (!warmGStreamer) warmGStreamer = std::make_unique<GStreamerRetained>(device.Get(), gstDevice);
                                        warmGStreamer->run(file, mailbox, stop);
                                    } else {
                                        GStreamerRetained fresh(device.Get(), gstDevice);
                                        fresh.run(file, mailbox, stop);
                                    }
                                    break;
                                }
                            }
                        } catch (const std::exception& e) { mailbox.fail(e.what()); }
                        mailbox.finished();
                    });
                    std::optional<Clock::time_point> presentedAt;
                    std::string path;
                    int frameCount = 0;
                    const auto deadline = openedAt + std::chrono::milliseconds(options.cycleMs);
                    while (Clock::now() < deadline && !quit) {
                        SDL_Event event;
                        while (SDL_PollEvent(&event)) if (event.type == SDL_EVENT_QUIT) quit = true;
                        if (auto frame = mailbox.take()) {
                            {
                                std::lock_guard lock(graphicsMutex);
                                if (backend == Backend::GStreamer) gst_d3d11_device_lock(gstDevice);
                                try { presenter.present(*frame); }
                                catch (...) {
                                    if (backend == Backend::GStreamer) gst_d3d11_device_unlock(gstDevice);
                                    throw;
                                }
                                if (backend == Backend::GStreamer) gst_d3d11_device_unlock(gstDevice);
                            }
                            if (!presentedAt) { presentedAt = Clock::now(); path = frame->path; }
                            ++frameCount;
                        }
                        if (!mailbox.error().empty() || (mailbox.done() && !presentedAt)) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const auto stoppingAt = Clock::now();
                    worker.request_stop();
                    worker.join();
                    const auto stoppedAt = Clock::now();
                    const auto decodedAt = mailbox.firstDecoded();
                    const std::string error = mailbox.error();
                    if (!presentedAt || !error.empty()) hadFailure = true;
                    const std::string decoded = decodedAt ? std::to_string(milliseconds(openedAt, *decodedAt)) : "";
                    const std::string presented = presentedAt ? std::to_string(milliseconds(openedAt, *presentedAt)) : "";
                    const std::string group = std::string(backendName(backend)) + "/" + (reuse ? "reuse" : "fresh");
                    if (presentedAt) presentationTimes[group].push_back(milliseconds(openedAt, *presentedAt));
                    else ++misses[group];
                    const double stopMs = milliseconds(stoppingAt, stoppedAt);
                    csv << round << ',' << csvCell(file) << ',' << backendName(backend) << ','
                        << (reuse ? "reuse" : "fresh") << ','
                        << decoded << ',' << presented << ',' << stopMs << ',' << frameCount << ','
                        << csvCell(path) << ',' << csvCell(error.empty() && !presentedAt ? "no first frame in cycle" : error) << '\n';
                    csv.flush();
                    std::cout << "round=" << round << " backend=" << backendName(backend)
                              << " mode=" << (reuse ? "reuse" : "fresh")
                              << " decoded=" << (decoded.empty() ? "MISS" : decoded)
                              << "ms presented=" << (presented.empty() ? "MISS" : presented)
                              << "ms stop=" << stopMs << "ms frames=" << frameCount
                              << " path=" << path << (error.empty() ? "" : " error=" + error) << '\n';
                    presenter.black();
                    if (quit) break;
                  }
                  if (quit) break;
                }
                if (quit) break;
            }
        }
        const auto finalStop = Clock::now();
        warmGStreamer.reset();
        warmFFmpeg.reset();
        warmMediaFoundation.reset();
        std::cout << "final warm-backend shutdown=" << milliseconds(finalStop, Clock::now()) << "ms\n";
        for (auto& [group, times] : presentationTimes) {
            std::sort(times.begin(), times.end());
            const size_t middle = times.size() / 2;
            const double median = times.size() % 2 ? times[middle] : (times[middle - 1] + times[middle]) / 2;
            std::cout << "summary " << group << " first-present median=" << median
                      << "ms n=" << times.size() << " misses=" << misses[group] << '\n';
        }
        for (const auto& [group, count] : misses)
            if (!presentationTimes.contains(group))
                std::cout << "summary " << group << " no first frame; misses=" << count << '\n';
        std::cout << "CSV: " << options.csv << '\n';
        return quit ? 130 : (hadFailure ? 2 : 0);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
