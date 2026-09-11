#include <SDL3/SDL.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string media;
    bool hidden = false;
    bool vsync = false;
    bool bounce = false;
    bool overlay = false;
    int exitAfterSeconds = 0;
};

std::mutex gLogMutex;

template <typename... Args>
void logLine(Args&&... args) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    (std::cout << ... << std::forward<Args>(args)) << '\n';
}

std::uint64_t processCpuTime100ns() {
    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetProcessTimes(
            GetCurrentProcess(),
            &creationTime,
            &exitTime,
            &kernelTime,
            &userTime)) {
        return 0;
    }

    ULARGE_INTEGER kernel{};
    kernel.LowPart = kernelTime.dwLowDateTime;
    kernel.HighPart = kernelTime.dwHighDateTime;
    ULARGE_INTEGER user{};
    user.LowPart = userTime.dwLowDateTime;
    user.HighPart = userTime.dwHighDateTime;
    return kernel.QuadPart + user.QuadPart;
}

void printUsage(const char* executable) {
    std::cout
        << "Usage:\n"
        << "  " << executable << " <video-file-or-uri> [options]\n\n"
        << "Options:\n"
        << "  --seconds N          Exit automatically after N seconds\n"
        << "  --hidden             Create a hidden test window\n"
        << "  --bounce             Scale video down and bounce it at 60 Hz\n"
        << "  --overlay            Bounce a small SDL RGBA texture above the video\n"
        << "  --vsync              Enable swap-chain vsync (diagnostic)\n"
        << "  --help               Show this help\n";
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);

        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return std::nullopt;
        }
        if (argument == "--hidden") {
            options.hidden = true;
            continue;
        }
        if (argument == "--bounce") {
            options.bounce = true;
            continue;
        }
        if (argument == "--overlay") {
            options.overlay = true;
            continue;
        }
        if (argument == "--vsync") {
            options.vsync = true;
            continue;
        }
        if (argument == "--seconds") {
            if (++i >= argc) {
                logLine("Missing value after --seconds");
                return std::nullopt;
            }

            try {
                options.exitAfterSeconds = std::max(0, std::stoi(argv[i]));
            } catch (...) {
                logLine("Invalid --seconds value: ", argv[i]);
                return std::nullopt;
            }
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            logLine("Unknown option: ", argument);
            return std::nullopt;
        }
        if (!options.media.empty()) {
            logLine("Only one media path or URI may be specified.");
            return std::nullopt;
        }

        options.media = std::string(argument);
    }

    if (options.media.empty()) {
        printUsage(argv[0]);
        return std::nullopt;
    }

    return options;
}

std::string makeUri(const std::string& input) {
    if (gst_uri_is_valid(input.c_str())) {
        return input;
    }

    GError* error = nullptr;
    gchar* uri = gst_filename_to_uri(input.c_str(), &error);
    if (!uri) {
        const std::string message =
            error && error->message ? error->message : "unknown URI conversion error";
        if (error) {
            g_error_free(error);
        }
        throw std::runtime_error("Could not convert media path to URI: " + message);
    }

    std::string result(uri);
    g_free(uri);
    return result;
}

const char* dxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_NV12:
            return "DXGI_FORMAT_NV12";
        default:
            return "other";
    }
}

void configureD3d11DecoderPreference() {
    constexpr const char* d3d12Decoders[] = {
        "d3d12h264dec",
        "d3d12h265dec",
        "d3d12vp8dec",
        "d3d12vp9dec",
        "d3d12mpeg2dec",
        "d3d12av1dec",
    };
    constexpr const char* d3d11Decoders[] = {
        "d3d11h264dec",
        "d3d11h265dec",
        "d3d11vp8dec",
        "d3d11vp9dec",
        "d3d11mpeg2dec",
        "d3d11av1dec",
    };
    constexpr const char* nvcodecDecoders[] = {
        "nvh264dec",
        "nvh265dec",
        "nvvp8dec",
        "nvvp9dec",
        "nvmpeg2videodec",
        "nvav1dec",
    };

    GstRegistry* registry = gst_registry_get();
    for (const char* name : d3d12Decoders) {
        if (GstPluginFeature* feature =
                gst_registry_lookup_feature(registry, name)) {
            gst_plugin_feature_set_rank(feature, GST_RANK_NONE);
            gst_object_unref(feature);
        }
    }
    for (const char* name : d3d11Decoders) {
        if (GstPluginFeature* feature =
                gst_registry_lookup_feature(registry, name)) {
            gst_plugin_feature_set_rank(
                feature, GST_RANK_PRIMARY + 100);
            gst_object_unref(feature);
        }
    }
    for (const char* name : nvcodecDecoders) {
        if (GstPluginFeature* feature =
                gst_registry_lookup_feature(registry, name)) {
            gst_plugin_feature_set_rank(feature, GST_RANK_NONE);
            gst_object_unref(feature);
        }
    }

    logLine(
        "Decoder preference: D3D11 enabled; D3D12 and NVDEC decoders "
        "disabled for shared-device interop");
}

SDL_Colorspace sdlColorspaceFor(const GstVideoInfo& info) {
    const bool fullRange =
        info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;

    switch (info.colorimetry.matrix) {
        case GST_VIDEO_COLOR_MATRIX_BT601:
            return fullRange ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
        case GST_VIDEO_COLOR_MATRIX_BT2020:
            return fullRange ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
        case GST_VIDEO_COLOR_MATRIX_BT709:
        default:
            return fullRange ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    }
}

class Nv12CopyPlayer {
public:
    Nv12CopyPlayer(SDL_Renderer* renderer, ID3D11Device* sdlDevice, Options options)
        : renderer_(renderer),
          sdlDevice_(sdlDevice),
          options_(std::move(options)) {
    }

    ~Nv12CopyPlayer() {
        shutdown();
    }

    Nv12CopyPlayer(const Nv12CopyPlayer&) = delete;
    Nv12CopyPlayer& operator=(const Nv12CopyPlayer&) = delete;

    bool start() {
        if (!initializeD3d11Synchronization()) {
            return false;
        }

        if (options_.overlay && !createOverlayTexture()) {
            return false;
        }

        gstDevice_ = gst_d3d11_device_new_wrapped(sdlDevice_);
        if (!gstDevice_) {
            logLine("gst_d3d11_device_new_wrapped failed");
            return false;
        }

        gstContext_ = gst_d3d11_context_new(gstDevice_);
        if (!gstContext_) {
            logLine("gst_d3d11_context_new failed");
            return false;
        }

        videoSinkBin_ = createVideoSinkBin();
        if (!videoSinkBin_) {
            return false;
        }

        pipeline_ = createPlaybinPipeline();
        if (!pipeline_) {
            return false;
        }

        gst_element_set_context(pipeline_, gstContext_);
        gst_element_set_context(videoSinkBin_, gstContext_);

        bus_ = gst_element_get_bus(pipeline_);
        gst_bus_set_sync_handler(bus_, &Nv12CopyPlayer::onBusSync, this, nullptr);

        const GstStateChangeReturn result =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (result == GST_STATE_CHANGE_FAILURE) {
            logLine("Pipeline refused GST_STATE_PLAYING");
            return false;
        }

        logLine(
            "Pipeline started: source=",
            options_.media,
            ", path=D3D11 NV12 GPU copy");
        measurementStart_ = std::chrono::steady_clock::now();
        cpuStart100ns_ = processCpuTime100ns();
        return true;
    }

    void pumpBus() {
        if (!bus_) {
            return;
        }

        while (GstMessage* message = gst_bus_pop(bus_)) {
            switch (GST_MESSAGE_TYPE(message)) {
                case GST_MESSAGE_ERROR: {
                    GError* error = nullptr;
                    gchar* debug = nullptr;
                    gst_message_parse_error(message, &error, &debug);
                    logLine(
                        "GStreamer error from ",
                        GST_OBJECT_NAME(GST_MESSAGE_SRC(message)),
                        ": ",
                        error && error->message ? error->message : "unknown error");
                    if (debug) {
                        logLine("  debug: ", debug);
                    }
                    if (error) {
                        g_error_free(error);
                    }
                    g_free(debug);
                    failed_ = true;
                    finished_ = true;
                    break;
                }
                case GST_MESSAGE_EOS:
                    logLine("End of stream");
                    finished_ = true;
                    break;
                case GST_MESSAGE_STATE_CHANGED:
                    if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline_)) {
                        GstState oldState = GST_STATE_VOID_PENDING;
                        GstState newState = GST_STATE_VOID_PENDING;
                        GstState pending = GST_STATE_VOID_PENDING;
                        gst_message_parse_state_changed(
                            message, &oldState, &newState, &pending);
                        logLine(
                            "Pipeline state: ",
                            gst_element_state_get_name(oldState),
                            " -> ",
                            gst_element_state_get_name(newState));
                    }
                    break;
                default:
                    break;
            }
            gst_message_unref(message);
        }
    }

    bool latchLatestSampleForFrame() {
        GstSample* sample = nullptr;
        {
            std::lock_guard<std::mutex> lock(sampleMutex_);
            sample = stagedSample_;
            stagedSample_ = nullptr;
        }

        if (!sample) {
            return false;
        }

        const auto binding = bindSample(sample);
        if (!binding) {
            ++rejectedFrames_;
            gst_sample_unref(sample);
            return false;
        }

        if (samplePendingRetirement_) {
            // A normal frame presents after every successful latch. Reaching
            // this branch means presentation was skipped, so keep the newest
            // retired sample and release the older one rather than growing an
            // unbounded queue.
            gst_sample_unref(samplePendingRetirement_);
            ++forcedRetiredSampleReleases_;
        }
        samplePendingRetirement_ = currentSample_;
        currentSample_ = sample;
        currentTexture_ = binding->texture;
        currentVisibleWidth_ = binding->visibleWidth;
        currentVisibleHeight_ = binding->visibleHeight;
        if (acceptedFrames_ == 0) {
            activeMeasurementStart_ =
                std::chrono::steady_clock::now();
            activeCpuStart100ns_ = processCpuTime100ns();
            activeMeasurementStarted_ = true;
        }
        ++acceptedFrames_;
        return true;
    }

    void drawFrame() {
        SDL_SetRenderDrawColor(renderer_, 10, 12, 16, 255);
        SDL_RenderClear(renderer_);

        int outputWidth = 0;
        int outputHeight = 0;
        const bool validOutput =
            SDL_GetCurrentRenderOutputSize(
                renderer_, &outputWidth, &outputHeight) &&
            outputWidth > 0 && outputHeight > 0;

        if (currentTexture_ && validOutput &&
            currentVisibleWidth_ > 0 && currentVisibleHeight_ > 0) {
            const float sourceAspect =
                static_cast<float>(currentVisibleWidth_) /
                static_cast<float>(currentVisibleHeight_);

            SDL_FRect destination{};
            const float availableWidth =
                static_cast<float>(outputWidth) *
                (options_.bounce ? 0.45f : 1.0f);
            const float availableHeight =
                static_cast<float>(outputHeight) *
                (options_.bounce ? 0.45f : 1.0f);
            const float availableAspect = availableWidth / availableHeight;

            if (sourceAspect > availableAspect) {
                destination.w = availableWidth;
                destination.h = destination.w / sourceAspect;
            } else {
                destination.h = availableHeight;
                destination.w = destination.h * sourceAspect;
            }

            if (options_.bounce) {
                const double elapsedSeconds =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - animationStart_)
                        .count();
                const float travelX =
                    static_cast<float>(outputWidth) - destination.w;
                const float travelY =
                    static_cast<float>(outputHeight) - destination.h;
                destination.x =
                    travelX * pingPong(elapsedSeconds, 4.8, 0.0);
                destination.y =
                    travelY * pingPong(elapsedSeconds, 3.6, 0.27);
            } else {
                destination.x =
                    (static_cast<float>(outputWidth) - destination.w) * 0.5f;
                destination.y =
                    (static_cast<float>(outputHeight) - destination.h) * 0.5f;
            }

            const SDL_FRect source{
                0.0f,
                0.0f,
                static_cast<float>(currentVisibleWidth_),
                static_cast<float>(currentVisibleHeight_)};
            if (!SDL_RenderTexture(
                    renderer_, currentTexture_, &source, &destination)) {
                logLine("SDL_RenderTexture failed: ", SDL_GetError());
                failed_ = true;
                finished_ = true;
            }
        }

        if (overlayTexture_ && validOutput) {
            const double elapsedSeconds =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - animationStart_)
                    .count();
            const float squareSize = std::clamp(
                static_cast<float>(std::min(outputWidth, outputHeight)) * 0.12f,
                48.0f,
                128.0f);
            const float travelX =
                std::max(0.0f, static_cast<float>(outputWidth) - squareSize);
            const float travelY =
                std::max(0.0f, static_cast<float>(outputHeight) - squareSize);
            const SDL_FRect overlayDestination{
                travelX * pingPong(elapsedSeconds, 2.7, 0.38),
                travelY * pingPong(elapsedSeconds, 3.9, 0.63),
                squareSize,
                squareSize};

            if (!SDL_RenderTexture(
                    renderer_, overlayTexture_, nullptr, &overlayDestination)) {
                logLine("SDL overlay RenderTexture failed: ", SDL_GetError());
                failed_ = true;
                finished_ = true;
            }
        }
    }

    void presentFrame() {
        const auto flushWaitStart = std::chrono::steady_clock::now();
        d3dMultithread_->Enter();
        const auto flushLockAcquired = std::chrono::steady_clock::now();
        const bool flushed = SDL_FlushRenderer(renderer_);
        d3dMultithread_->Leave();
        const auto flushLockReleased = std::chrono::steady_clock::now();

        const auto flushWaitNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                flushLockAcquired - flushWaitStart)
                .count();
        const auto flushHeldNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                flushLockReleased - flushLockAcquired)
                .count();
        flushLockWaitNanoseconds_ +=
            static_cast<std::uint64_t>(flushWaitNanoseconds);
        flushLockHeldNanoseconds_ +=
            static_cast<std::uint64_t>(flushHeldNanoseconds);
        maxFlushLockWaitNanoseconds_ = std::max(
            maxFlushLockWaitNanoseconds_,
            static_cast<std::uint64_t>(flushWaitNanoseconds));
        maxFlushLockHeldNanoseconds_ = std::max(
            maxFlushLockHeldNanoseconds_,
            static_cast<std::uint64_t>(flushHeldNanoseconds));

        if (!flushed) {
            logLine("SDL_FlushRenderer failed: ", SDL_GetError());
            failed_ = true;
            finished_ = true;
            return;
        }

        const auto presentStart = std::chrono::steady_clock::now();
        const bool presented = SDL_RenderPresent(renderer_);
        const auto presentEnd = std::chrono::steady_clock::now();
        const auto presentNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                presentEnd - presentStart)
                .count();
        presentNanoseconds_ +=
            static_cast<std::uint64_t>(presentNanoseconds);
        maxPresentNanoseconds_ = std::max(
            maxPresentNanoseconds_,
            static_cast<std::uint64_t>(presentNanoseconds));

        if (!presented) {
            logLine("SDL_RenderPresent failed: ", SDL_GetError());
            failed_ = true;
            finished_ = true;
            return;
        }

        if (samplePendingRetirement_) {
            gst_sample_unref(samplePendingRetirement_);
            samplePendingRetirement_ = nullptr;
            ++postPresentSampleReleases_;
        }
        ++presentedFrames_;
    }

    [[nodiscard]] bool finished() const {
        return finished_;
    }

    static float pingPong(
        double elapsedSeconds,
        double periodSeconds,
        double phase) {
        double position =
            std::fmod(elapsedSeconds / periodSeconds + phase, 1.0);
        if (position < 0.0) {
            position += 1.0;
        }
        return static_cast<float>(
            position < 0.5 ? position * 2.0 : (1.0 - position) * 2.0);
    }

    [[nodiscard]] bool failed() const {
        return failed_;
    }

    [[nodiscard]] bool hasFrame() const {
        return currentTexture_ != nullptr;
    }

    void printStats() const {
        const auto wallMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - measurementStart_)
                .count();
        const double cpuMilliseconds =
            static_cast<double>(
                processCpuTime100ns() - cpuStart100ns_) /
            10000.0;
        const auto averageCpuPercent =
            wallMilliseconds > 0
                ? static_cast<std::uint64_t>(
                      cpuMilliseconds * 100.0 /
                      static_cast<double>(wallMilliseconds))
                : 0;
        const auto activeWallMilliseconds =
            activeMeasurementStarted_
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() -
                      activeMeasurementStart_)
                      .count()
                : 0;
        const double activeCpuMilliseconds =
            activeMeasurementStarted_
                ? static_cast<double>(
                      processCpuTime100ns() -
                      activeCpuStart100ns_) /
                      10000.0
                : 0.0;
        const auto activeCpuPercent =
            activeWallMilliseconds > 0
                ? static_cast<std::uint64_t>(
                      activeCpuMilliseconds * 100.0 /
                      static_cast<double>(activeWallMilliseconds))
                : 0;

        logLine(
            "Stats: received=",
            receivedFrames_.load(),
            ", accepted=",
            acceptedFrames_,
            ", rejected=",
            rejectedFrames_,
            ", callback-dropped=",
            callbackDroppedFrames_.load(),
            ", presents=",
            presentedFrames_,
            ", gpu-copies=",
            copiedFrames_,
            ", SDL wrappers=",
            copyRing_.size(),
            ", post-present-releases=",
            postPresentSampleReleases_,
            ", forced-retire-releases=",
            forcedRetiredSampleReleases_,
            ", copy-native-lock-wait-us=",
            copyLockWaitNanoseconds_ / 1000,
            ", copy-native-lock-held-us=",
            copyLockHeldNanoseconds_ / 1000,
            ", max-copy-native-lock-wait-us=",
            maxCopyLockWaitNanoseconds_ / 1000,
            ", max-copy-native-lock-held-us=",
            maxCopyLockHeldNanoseconds_ / 1000,
            ", flush-native-lock-wait-us=",
            flushLockWaitNanoseconds_ / 1000,
            ", flush-native-lock-held-us=",
            flushLockHeldNanoseconds_ / 1000,
            ", max-flush-native-lock-wait-us=",
            maxFlushLockWaitNanoseconds_ / 1000,
            ", max-flush-native-lock-held-us=",
            maxFlushLockHeldNanoseconds_ / 1000,
            ", present-total-ms=",
            presentNanoseconds_ / 1000000,
            ", max-present-us=",
            maxPresentNanoseconds_ / 1000,
            ", wall-ms=",
            wallMilliseconds,
            ", process-cpu-ms=",
            static_cast<std::uint64_t>(cpuMilliseconds),
            ", average-process-cpu=",
            averageCpuPercent,
            "%",
            ", active-wall-ms=",
            activeWallMilliseconds,
            ", active-process-cpu-ms=",
            static_cast<std::uint64_t>(activeCpuMilliseconds),
            ", active-process-cpu=",
            activeCpuPercent,
            "%",
            ", hardware decoder seen=",
            hardwareDecoderSeen_.load() ? "yes" : "no");
    }

    void shutdown() {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;

        if (videoAppSink_) {
            GstAppSinkCallbacks callbacks{};
            gst_app_sink_set_callbacks(
                GST_APP_SINK(videoAppSink_), &callbacks, nullptr, nullptr);
        }

        {
            std::lock_guard<std::mutex> lock(sampleMutex_);
            if (stagedSample_) {
                gst_sample_unref(stagedSample_);
                stagedSample_ = nullptr;
            }
        }
        if (currentSample_) {
            gst_sample_unref(currentSample_);
            currentSample_ = nullptr;
        }
        if (samplePendingRetirement_) {
            gst_sample_unref(samplePendingRetirement_);
            samplePendingRetirement_ = nullptr;
        }
        currentTexture_ = nullptr;

        if (overlayTexture_) {
            SDL_DestroyTexture(overlayTexture_);
            overlayTexture_ = nullptr;
        }

        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }

        destroyCopyRing();

        if (bus_) {
            gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (pipeline_) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        videoSinkBin_ = nullptr;
        videoAppSink_ = nullptr;

        if (gstContext_) {
            gst_context_unref(gstContext_);
            gstContext_ = nullptr;
        }
        if (gstDevice_) {
            gst_object_unref(gstDevice_);
            gstDevice_ = nullptr;
        }
        if (d3dMultithread_) {
            d3dMultithread_->Release();
            d3dMultithread_ = nullptr;
        }
    }

private:
    bool initializeD3d11Synchronization() {
        ID3D11DeviceContext* immediateContext = nullptr;
        sdlDevice_->GetImmediateContext(&immediateContext);
        if (!immediateContext) {
            logLine("D3D11 device did not expose an immediate context");
            return false;
        }

        const HRESULT queryResult = immediateContext->QueryInterface(
            IID_PPV_ARGS(&d3dMultithread_));
        immediateContext->Release();
        if (FAILED(queryResult) || !d3dMultithread_) {
            logLine(
                "Could not query ID3D11Multithread: 0x",
                std::hex,
                static_cast<unsigned long>(queryResult),
                std::dec);
            return false;
        }

        const BOOL wasProtected =
            d3dMultithread_->SetMultithreadProtected(TRUE);
        logLine(
            "D3D11 immediate-context protection enabled "
            "(previously ",
            wasProtected ? "enabled" : "disabled",
            ")");
        return true;
    }

    bool createOverlayTexture() {
        constexpr int textureSize = 96;
        constexpr int bytesPerPixel = 4;

        overlayTexture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STATIC,
            textureSize,
            textureSize);
        if (!overlayTexture_) {
            logLine("SDL_CreateTexture for overlay failed: ", SDL_GetError());
            return false;
        }

        std::vector<std::uint8_t> pixels(
            textureSize * textureSize * bytesPerPixel);
        for (int y = 0; y < textureSize; ++y) {
            for (int x = 0; x < textureSize; ++x) {
                const bool border =
                    x < 4 || y < 4 ||
                    x >= textureSize - 4 || y >= textureSize - 4;
                const bool alternate = ((x / 12) + (y / 12)) % 2 != 0;
                const std::size_t offset =
                    static_cast<std::size_t>(
                        (y * textureSize + x) * bytesPerPixel);

                pixels[offset] = border ? 255 : (alternate ? 255 : 40);
                pixels[offset + 1] = border ? 255 : (alternate ? 70 : 220);
                pixels[offset + 2] = border ? 255 : (alternate ? 170 : 255);
                pixels[offset + 3] = 235;
            }
        }

        if (!SDL_UpdateTexture(
                overlayTexture_,
                nullptr,
                pixels.data(),
                textureSize * bytesPerPixel)) {
            logLine("SDL_UpdateTexture for overlay failed: ", SDL_GetError());
            SDL_DestroyTexture(overlayTexture_);
            overlayTexture_ = nullptr;
            return false;
        }
        if (!SDL_SetTextureBlendMode(overlayTexture_, SDL_BLENDMODE_BLEND)) {
            logLine(
                "SDL_SetTextureBlendMode for overlay failed: ",
                SDL_GetError());
            SDL_DestroyTexture(overlayTexture_);
            overlayTexture_ = nullptr;
            return false;
        }

        return true;
    }

    struct SampleBinding {
        SDL_Texture* texture = nullptr;
        int visibleWidth = 0;
        int visibleHeight = 0;
    };

    struct CopySlot {
        SDL_Texture* sdlTexture = nullptr;
        ID3D11Texture2D* nativeTexture = nullptr;
    };

    GstElement* createVideoSinkBin() {
        GstElement* bin = gst_bin_new("sdl3_video_sink_bin");

        videoAppSink_ = gst_element_factory_make("appsink", "video_sink");
        if (!videoAppSink_) {
            logLine("Could not create video appsink");
            gst_object_unref(bin);
            return nullptr;
        }

        GstCaps* caps = gst_caps_from_string(
            "video/x-raw(memory:D3D11Memory),"
            "format=NV12,pixel-aspect-ratio=1/1");

        g_object_set(
            videoAppSink_,
            "emit-signals",
            FALSE,
            "max-buffers",
            1,
            "drop",
            TRUE,
            "sync",
            TRUE,
            "qos",
            TRUE,
            "enable-last-sample",
            FALSE,
            "wait-on-eos",
            FALSE,
            "caps",
            caps,
            nullptr);
        gst_caps_unref(caps);

        GstAppSinkCallbacks callbacks{};
        callbacks.new_preroll = &Nv12CopyPlayer::onNewPreroll;
        callbacks.new_sample = &Nv12CopyPlayer::onNewSample;
        gst_app_sink_set_callbacks(
            GST_APP_SINK(videoAppSink_), &callbacks, this, nullptr);

        gst_bin_add(GST_BIN(bin), videoAppSink_);

        GstPad* sinkPad =
            gst_element_get_static_pad(videoAppSink_, "sink");
        GstPad* ghostPad = gst_ghost_pad_new("sink", sinkPad);
        gst_object_unref(sinkPad);
        if (!ghostPad || !gst_element_add_pad(bin, ghostPad)) {
            logLine("Could not create video sink bin ghost pad");
            if (ghostPad) {
                gst_object_unref(ghostPad);
            }
            gst_object_unref(bin);
            return nullptr;
        }

        gst_element_set_context(bin, gstContext_);
        return bin;
    }

    GstElement* createPlaybinPipeline() {
        GstElement* playbin = gst_element_factory_make("playbin", "player");
        if (!playbin) {
            logLine("Could not create playbin");
            return nullptr;
        }

        std::string uri;
        try {
            uri = makeUri(options_.media);
        } catch (const std::exception& error) {
            logLine(error.what());
            gst_object_unref(playbin);
            return nullptr;
        }

        constexpr gint kPlayFlagVideo = 1 << 0;
        g_object_set(
            playbin,
            "uri",
            uri.c_str(),
            "flags",
            kPlayFlagVideo,
            "video-sink",
            videoSinkBin_,
            nullptr);
        g_signal_connect(
            playbin,
            "element-setup",
            G_CALLBACK(&Nv12CopyPlayer::onElementSetup),
            this);
        return playbin;
    }

    std::optional<SampleBinding> bindSample(GstSample* sample) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!buffer || !caps) {
            logLine("Rejected sample without buffer or caps");
            return std::nullopt;
        }

        GstVideoInfo info{};
        if (!gst_video_info_from_caps(&info, caps)) {
            logLine("Rejected sample with invalid video caps");
            return std::nullopt;
        }

        if (!loggedCaps_) {
            gchar* capsText = gst_caps_to_string(caps);
            logLine("Negotiated caps: ", capsText ? capsText : "(null)");
            g_free(capsText);
            loggedCaps_ = true;
        }

        const guint memoryCount = gst_buffer_n_memory(buffer);
        if (memoryCount != 1) {
            if (unexpectedMemoryFrames_++ < 8) {
                logLine(
                    "Rejected frame: expected one D3D11 memory object, got ",
                    memoryCount);
            }
            return std::nullopt;
        }

        GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
        if (!memory || !gst_is_d3d11_memory(memory)) {
            logLine(
                "Rejected frame: caps claimed D3D11Memory but the buffer is not "
                "GstD3D11Memory");
            return std::nullopt;
        }

        auto* d3dMemory = GST_D3D11_MEMORY_CAST(memory);
        if (gst_d3d11_memory_get_native_type(d3dMemory) !=
            GST_D3D11_MEMORY_NATIVE_TYPE_TEXTURE_2D) {
            logLine("Rejected frame: D3D11 memory is not an ID3D11Texture2D");
            return std::nullopt;
        }

        ID3D11Device* memoryDevice =
            gst_d3d11_device_get_device_handle(d3dMemory->device);
        if (memoryDevice != sdlDevice_) {
            logLine(
                "Rejected frame: GStreamer and SDL are using different "
                "ID3D11Device objects");
            return std::nullopt;
        }

        auto* nativeTexture = static_cast<ID3D11Texture2D*>(
            gst_d3d11_memory_get_resource_handle(d3dMemory));
        if (!nativeTexture) {
            logLine("Rejected frame: GstD3D11Memory has no resource handle");
            return std::nullopt;
        }

        D3D11_TEXTURE2D_DESC description{};
        if (!gst_d3d11_memory_get_texture_desc(d3dMemory, &description)) {
            logLine("Rejected frame: could not query D3D11 texture description");
            return std::nullopt;
        }

        const guint subresource =
            gst_d3d11_memory_get_subresource_index(d3dMemory);

        if (description.Format != DXGI_FORMAT_NV12) {
            logLine(
                "Rejected NV12 copy source with native format ",
                dxgiFormatName(description.Format));
            return std::nullopt;
        }

        return copyNv12Sample(
            info,
            d3dMemory->device,
            nativeTexture,
            description,
            subresource);
    }

    std::optional<SampleBinding> copyNv12Sample(
        const GstVideoInfo& info,
        GstD3D11Device* sourceDevice,
        ID3D11Texture2D* source,
        const D3D11_TEXTURE2D_DESC& sourceDescription,
        guint sourceSubresource) {
        if (!ensureCopyRing(info, sourceDescription)) {
            return std::nullopt;
        }

        CopySlot& slot = copyRing_[copyRingIndex_];
        copyRingIndex_ = (copyRingIndex_ + 1) % copyRing_.size();

        ID3D11DeviceContext* deviceContext =
            gst_d3d11_device_get_device_context_handle(sourceDevice);
        if (!deviceContext) {
            logLine("Shared GstD3D11Device has no immediate context");
            return std::nullopt;
        }

        const auto copyLockWaitStart = std::chrono::steady_clock::now();
        d3dMultithread_->Enter();
        const auto copyLockAcquired = std::chrono::steady_clock::now();
        deviceContext->CopySubresourceRegion(
            slot.nativeTexture,
            0,
            0,
            0,
            0,
            source,
            sourceSubresource,
            nullptr);
        d3dMultithread_->Leave();
        const auto copyLockReleased = std::chrono::steady_clock::now();

        const auto copyLockWaitNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                copyLockAcquired - copyLockWaitStart)
                .count();
        const auto copyLockHeldNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                copyLockReleased - copyLockAcquired)
                .count();
        copyLockWaitNanoseconds_ +=
            static_cast<std::uint64_t>(copyLockWaitNanoseconds);
        copyLockHeldNanoseconds_ +=
            static_cast<std::uint64_t>(copyLockHeldNanoseconds);
        maxCopyLockWaitNanoseconds_ = std::max(
            maxCopyLockWaitNanoseconds_,
            static_cast<std::uint64_t>(copyLockWaitNanoseconds));
        maxCopyLockHeldNanoseconds_ = std::max(
            maxCopyLockHeldNanoseconds_,
            static_cast<std::uint64_t>(copyLockHeldNanoseconds));

        if (copiedFrames_++ < 8) {
            logLine(
                "NV12 GPU copy: source ArraySize=",
                sourceDescription.ArraySize,
                ", source subresource=",
                sourceSubresource,
                ", destination slot=",
                copyRingIndex_ == 0 ? copyRing_.size() - 1
                                    : copyRingIndex_ - 1);
        }

        return SampleBinding{
            slot.sdlTexture,
            GST_VIDEO_INFO_WIDTH(&info),
            GST_VIDEO_INFO_HEIGHT(&info)};
    }

    bool ensureCopyRing(
        const GstVideoInfo& info,
        const D3D11_TEXTURE2D_DESC& sourceDescription) {
        if (!copyRing_.empty() &&
            copyRingWidth_ == sourceDescription.Width &&
            copyRingHeight_ == sourceDescription.Height) {
            return true;
        }

        destroyCopyRing();

        D3D11_TEXTURE2D_DESC destinationDescription{};
        destinationDescription.Width = sourceDescription.Width;
        destinationDescription.Height = sourceDescription.Height;
        destinationDescription.MipLevels = 1;
        destinationDescription.ArraySize = 1;
        destinationDescription.Format = DXGI_FORMAT_NV12;
        destinationDescription.SampleDesc.Count = 1;
        destinationDescription.Usage = D3D11_USAGE_DEFAULT;
        destinationDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        constexpr std::size_t kCopyRingSize = 3;
        const SDL_Colorspace colorspace = sdlColorspaceFor(info);

        for (std::size_t i = 0; i < kCopyRingSize; ++i) {
            ID3D11Texture2D* nativeTexture = nullptr;
            const HRESULT createResult = sdlDevice_->CreateTexture2D(
                &destinationDescription, nullptr, &nativeTexture);
            if (FAILED(createResult) || !nativeTexture) {
                logLine(
                    "ID3D11Device::CreateTexture2D for NV12 copy target "
                    "failed: HRESULT=0x",
                    std::hex,
                    static_cast<unsigned long>(createResult),
                    std::dec);
                destroyCopyRing();
                return false;
            }

            SDL_PropertiesID properties = SDL_CreateProperties();
            const bool propertiesOk =
                properties != 0 &&
                SDL_SetNumberProperty(
                    properties,
                    SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                    SDL_PIXELFORMAT_NV12) &&
                SDL_SetNumberProperty(
                    properties,
                    SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                    SDL_TEXTUREACCESS_STATIC) &&
                SDL_SetNumberProperty(
                    properties,
                    SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                    destinationDescription.Width) &&
                SDL_SetNumberProperty(
                    properties,
                    SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                    destinationDescription.Height) &&
                SDL_SetNumberProperty(
                    properties,
                    SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                    static_cast<Sint64>(colorspace)) &&
                SDL_SetPointerProperty(
                    properties,
                    SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,
                    nativeTexture);

            SDL_Texture* sdlTexture =
                propertiesOk
                    ? SDL_CreateTextureWithProperties(renderer_, properties)
                    : nullptr;
            if (properties) {
                SDL_DestroyProperties(properties);
            }
            if (!sdlTexture ||
                !SDL_SetTextureScaleMode(
                    sdlTexture, SDL_SCALEMODE_LINEAR)) {
                logLine(
                    "Could not wrap NV12 copy target in SDL3: ",
                    SDL_GetError());
                if (sdlTexture) {
                    SDL_DestroyTexture(sdlTexture);
                }
                nativeTexture->Release();
                destroyCopyRing();
                return false;
            }

            copyRing_.push_back({sdlTexture, nativeTexture});
        }

        copyRingWidth_ = destinationDescription.Width;
        copyRingHeight_ = destinationDescription.Height;
        copyRingIndex_ = 0;
        logLine(
            "Created ",
            copyRing_.size(),
            " shader-readable NV12 copy targets: ",
            copyRingWidth_,
            "x",
            copyRingHeight_,
            ", bindFlags=0x",
            std::hex,
            destinationDescription.BindFlags,
            std::dec);
        return true;
    }

    void destroyCopyRing() {
        for (CopySlot& slot : copyRing_) {
            if (slot.sdlTexture) {
                SDL_DestroyTexture(slot.sdlTexture);
            }
            if (slot.nativeTexture) {
                slot.nativeTexture->Release();
            }
        }
        copyRing_.clear();
        copyRingWidth_ = 0;
        copyRingHeight_ = 0;
        copyRingIndex_ = 0;
    }

    static GstFlowReturn stageSample(
        GstAppSink* sink, Nv12CopyPlayer* player, bool preroll) {
        GstSample* sample =
            preroll ? gst_app_sink_pull_preroll(sink)
                    : gst_app_sink_pull_sample(sink);
        if (!sample) {
            return GST_FLOW_OK;
        }

        ++player->receivedFrames_;
        {
            std::lock_guard<std::mutex> lock(player->sampleMutex_);
            if (player->stagedSample_) {
                gst_sample_unref(player->stagedSample_);
                ++player->callbackDroppedFrames_;
            }
            player->stagedSample_ = sample;
        }
        return GST_FLOW_OK;
    }

    static GstFlowReturn onNewPreroll(
        GstAppSink* sink, gpointer userData) {
        return stageSample(
            sink, static_cast<Nv12CopyPlayer*>(userData), true);
    }

    static GstFlowReturn onNewSample(
        GstAppSink* sink, gpointer userData) {
        return stageSample(
            sink, static_cast<Nv12CopyPlayer*>(userData), false);
    }

    static GstBusSyncReply onBusSync(
        GstBus*, GstMessage* message, gpointer userData) {
        auto* player = static_cast<Nv12CopyPlayer*>(userData);
        if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_NEED_CONTEXT) {
            return GST_BUS_PASS;
        }

        const gchar* contextType = nullptr;
        gst_message_parse_context_type(message, &contextType);
        if (contextType &&
            std::string_view(contextType) ==
                GST_D3D11_DEVICE_HANDLE_CONTEXT_TYPE) {
            gst_element_set_context(
                GST_ELEMENT(GST_MESSAGE_SRC(message)), player->gstContext_);
            logLine(
                "Provided SDL renderer's D3D11 device to ",
                GST_OBJECT_NAME(GST_MESSAGE_SRC(message)));
        }
        return GST_BUS_PASS;
    }

    static void onElementSetup(
        GstElement*, GstElement* element, gpointer userData) {
        auto* player = static_cast<Nv12CopyPlayer*>(userData);
        GstElementFactory* factory = gst_element_get_factory(element);
        if (!factory) {
            return;
        }

        const gchar* klass = gst_element_factory_get_metadata(
            factory, GST_ELEMENT_METADATA_KLASS);
        const gchar* factoryName =
            gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        if (!klass || !factoryName) {
            return;
        }

        const std::string_view klassView(klass);
        if (klassView.find("Codec/Decoder/Video") == std::string_view::npos) {
            return;
        }

        const bool d3d11Hardware =
            std::string_view(factoryName).starts_with("d3d11");
        if (d3d11Hardware) {
            player->hardwareDecoderSeen_ = true;
        }
        logLine(
            "Decoder selected: ",
            factoryName,
            " [",
            klass,
            "]",
            d3d11Hardware
                ? " (D3D11 hardware)"
                : " (not a D3D11 hardware decoder)");
    }

    SDL_Renderer* renderer_ = nullptr;
    ID3D11Device* sdlDevice_ = nullptr;
    Options options_;

    GstElement* pipeline_ = nullptr;
    GstElement* videoSinkBin_ = nullptr;
    GstElement* videoAppSink_ = nullptr;
    GstBus* bus_ = nullptr;
    GstD3D11Device* gstDevice_ = nullptr;
    GstContext* gstContext_ = nullptr;
    ID3D11Multithread* d3dMultithread_ = nullptr;

    std::mutex sampleMutex_;
    GstSample* stagedSample_ = nullptr;
    GstSample* currentSample_ = nullptr;
    GstSample* samplePendingRetirement_ = nullptr;

    std::vector<CopySlot> copyRing_;
    UINT copyRingWidth_ = 0;
    UINT copyRingHeight_ = 0;
    std::size_t copyRingIndex_ = 0;
    SDL_Texture* currentTexture_ = nullptr;
    SDL_Texture* overlayTexture_ = nullptr;
    int currentVisibleWidth_ = 0;
    int currentVisibleHeight_ = 0;
    std::chrono::steady_clock::time_point animationStart_ =
        std::chrono::steady_clock::now();

    std::atomic<std::uint64_t> receivedFrames_{0};
    std::atomic<std::uint64_t> callbackDroppedFrames_{0};
    std::atomic<bool> hardwareDecoderSeen_{false};
    std::uint64_t acceptedFrames_ = 0;
    std::uint64_t rejectedFrames_ = 0;
    std::uint64_t unexpectedMemoryFrames_ = 0;
    std::uint64_t copiedFrames_ = 0;
    std::uint64_t presentedFrames_ = 0;
    std::uint64_t postPresentSampleReleases_ = 0;
    std::uint64_t forcedRetiredSampleReleases_ = 0;
    std::uint64_t copyLockWaitNanoseconds_ = 0;
    std::uint64_t copyLockHeldNanoseconds_ = 0;
    std::uint64_t maxCopyLockWaitNanoseconds_ = 0;
    std::uint64_t maxCopyLockHeldNanoseconds_ = 0;
    std::uint64_t flushLockWaitNanoseconds_ = 0;
    std::uint64_t flushLockHeldNanoseconds_ = 0;
    std::uint64_t maxFlushLockWaitNanoseconds_ = 0;
    std::uint64_t maxFlushLockHeldNanoseconds_ = 0;
    std::uint64_t presentNanoseconds_ = 0;
    std::uint64_t maxPresentNanoseconds_ = 0;

    std::chrono::steady_clock::time_point measurementStart_ =
        std::chrono::steady_clock::now();
    std::uint64_t cpuStart100ns_ = processCpuTime100ns();
    std::chrono::steady_clock::time_point activeMeasurementStart_ =
        std::chrono::steady_clock::now();
    std::uint64_t activeCpuStart100ns_ = processCpuTime100ns();
    bool activeMeasurementStarted_ = false;

    bool loggedCaps_ = false;
    bool finished_ = false;
    bool failed_ = false;
    bool shutdown_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    const auto options = parseOptions(argc, argv);
    if (!options) {
        return argc > 1 ? 0 : 2;
    }

    gst_init(&argc, &argv);
    configureD3d11DecoderPreference();

    if (!SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1")) {
        logLine("Could not set SDL D3D11 thread-safety hint");
        gst_deinit();
        return 1;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        logLine("SDL_Init failed: ", SDL_GetError());
        gst_deinit();
        return 1;
    }

    const SDL_WindowFlags windowFlags =
        static_cast<SDL_WindowFlags>(
            SDL_WINDOW_RESIZABLE |
            (options->hidden ? SDL_WINDOW_HIDDEN : 0));
    SDL_Window* window = SDL_CreateWindow(
        "GStreamer + stock SDL3 D3D11 NV12 copy example",
        1280,
        720,
        windowFlags);
    if (!window) {
        logLine("SDL_CreateWindow failed: ", SDL_GetError());
        SDL_Quit();
        gst_deinit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, "direct3d11");
    if (!renderer) {
        logLine(
            "SDL_CreateRenderer(direct3d11) failed: ",
            SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        gst_deinit();
        return 1;
    }

    if (!SDL_SetRenderVSync(renderer, options->vsync ? 1 : 0)) {
        logLine("SDL_SetRenderVSync failed: ", SDL_GetError());
    }

    const SDL_PropertiesID rendererProperties =
        SDL_GetRendererProperties(renderer);
    auto* sdlDevice = static_cast<ID3D11Device*>(
        SDL_GetPointerProperty(
            rendererProperties,
            SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,
            nullptr));
    if (!sdlDevice) {
        logLine("SDL renderer did not expose an ID3D11Device");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        gst_deinit();
        return 1;
    }
    logLine(
        "SDL renderer: ",
        SDL_GetRendererName(renderer),
        ", D3D11 device=",
        static_cast<void*>(sdlDevice),
        ", path=D3D11 NV12 GPU copy");

    int exitCode = 0;
    {
        Nv12CopyPlayer player(renderer, sdlDevice, *options);
        if (!player.start()) {
            exitCode = 1;
        } else {
            const auto startTime = std::chrono::steady_clock::now();
            const auto animationFramePeriod =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / 60.0));
            auto nextAnimationFrame = startTime;
            bool quit = false;
            bool redrawRequested = false;

            while (!quit && !player.finished()) {
                SDL_Event event{};
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_EVENT_QUIT ||
                        (event.type == SDL_EVENT_KEY_DOWN &&
                         event.key.key == SDLK_ESCAPE)) {
                        quit = true;
                    }
                    if (event.type == SDL_EVENT_WINDOW_EXPOSED ||
                        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                        event.type == SDL_EVENT_WINDOW_RESTORED ||
                        event.type == SDL_EVENT_WINDOW_SHOWN) {
                        redrawRequested = true;
                    }
                }

                player.pumpBus();
                // RetroFE-style preparation boundary: latch media once before
                // any drawing, then keep the selected texture stable for the
                // complete visual frame.
                const bool newFrame =
                    player.latchLatestSampleForFrame();
                const auto now = std::chrono::steady_clock::now();
                const bool animated =
                    options->bounce || options->overlay;
                const bool animationFrameDue =
                    animated && now >= nextAnimationFrame;
                if (player.hasFrame() &&
                    (redrawRequested ||
                     (animated ? animationFrameDue : newFrame))) {
                    player.drawFrame();
                    if (!player.finished()) {
                        player.presentFrame();
                    }
                    redrawRequested = false;

                    if (animationFrameDue) {
                        do {
                            nextAnimationFrame += animationFramePeriod;
                        } while (nextAnimationFrame <= now);
                    }
                } else {
                    SDL_Delay(1);
                }

                if (options->exitAfterSeconds > 0) {
                    const auto elapsed = now - startTime;
                    if (elapsed >= std::chrono::seconds(
                                       options->exitAfterSeconds)) {
                        quit = true;
                    }
                }

            }

            player.printStats();
            if (player.failed()) {
                exitCode = 1;
            }
        }
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    gst_deinit();
    return exitCode;
}
