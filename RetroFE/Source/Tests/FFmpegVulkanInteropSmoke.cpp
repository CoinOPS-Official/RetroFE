#include "../Video/VulkanVideoInterop.h"
#include <SDL3/SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}
#include <cstdio>
#include <algorithm>
#include <memory>
#include <set>

int main(int argc, char** argv) {
    constexpr int targetFrames = 90;
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: retrofe_ffmpeg_vulkan_interop_smoke <video> [readback.bmp]\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    const int outputW = argc == 3 ? 1920 : 256;
    const int outputH = argc == 3 ? 1080 : 144;
    SDL_Window* window = SDL_CreateWindow("FFmpeg Vulkan smoke", outputW, outputH,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) return 1;
    SDL_Renderer* renderer = VulkanVideoInterop::createRenderer(window);
    if (!renderer) return 1;
    AVBufferRef* hardware = VulkanVideoInterop::createFFmpegDevice(renderer);
    if (!hardware) return 1;
    auto interop = std::make_unique<VulkanVideoInterop>(renderer, true);
    auto secondInterop = std::make_unique<VulkanVideoInterop>(renderer, true);
    SDL_Texture* heldTexture = nullptr;
    SDL_Texture* latestTexture = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int presented = 0;
    std::set<SDL_Texture*> wrappers;
    bool good = packet && frame && avformat_open_input(&format, argv[1], nullptr, nullptr) >= 0 &&
        avformat_find_stream_info(format, nullptr) >= 0;
    VulkanVideoInterop::DeviceInfo deviceInfo;
    auto* hardwareContext = reinterpret_cast<AVHWDeviceContext*>(hardware->data);
    auto* vulkanContext = static_cast<AVVulkanDeviceContext*>(hardwareContext->hwctx);
    auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(vulkanContext->get_proc_addr(
        vulkanContext->inst,"vkGetDeviceQueue"));
    VkQueue ffmpegGraphics = VK_NULL_HANDLE;
    good = good && VulkanVideoInterop::sharedDevice(renderer,deviceInfo) && getQueue;
    if (good) getQueue(vulkanContext->act_dev,deviceInfo.graphicsFamily,0,&ffmpegGraphics);
    good = good && ffmpegGraphics == deviceInfo.videoQueue && ffmpegGraphics != deviceInfo.graphicsQueue;
    if (!good) std::fprintf(stderr,"FFmpeg must use the reserved graphics queue, independently of SDL\n");
    int stream = -1;
    if (good) stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) good = false;
    if (good) {
        const AVCodec* codec = avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
        decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
        good = decoder && avcodec_parameters_to_context(decoder,
            format->streams[stream]->codecpar) >= 0;
        if (good) {
            decoder->hw_device_ctx = av_buffer_ref(hardware);
            decoder->get_format = [](AVCodecContext*, const AVPixelFormat* formats) {
                for (const auto* pixel = formats; *pixel != AV_PIX_FMT_NONE; ++pixel)
                    if (*pixel == AV_PIX_FMT_VULKAN) return *pixel;
                return AV_PIX_FMT_NONE;
            };
            good = avcodec_open2(decoder, codec, nullptr) >= 0;
        }
    }
    auto receive = [&] {
        while (good && presented < targetFrames) {
            const int status = avcodec_receive_frame(decoder, frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            if (status < 0 || frame->format != AV_PIX_FMT_VULKAN) {
                good = false;
                break;
            }
            if (presented == 0) {
                auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
                auto* vkFrames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
                auto* vk = reinterpret_cast<AVVkFrame*>(frame->data[0]);
                vkFrames->lock_frame(frames, vk);
                const uint64_t saved = vk->sem_value[0];
                vk->sem_value[0] = saved + 1000000;
                vkFrames->unlock_frame(frames, vk);
                const Uint64 start = SDL_GetTicksNS();
                good = !interop->copy(frame, SDL_COLORSPACE_BT709_LIMITED) &&
                    interop->deferred() && SDL_GetTicksNS() - start < 250000000ULL;
                vkFrames->lock_frame(frames, vk);
                vk->sem_value[0] = saved;
                vkFrames->unlock_frame(frames, vk);
                if (!good) { std::fprintf(stderr, "Delayed FFmpeg producer was not deferred\n"); break; }
            }
            auto importReady = [&](VulkanVideoInterop& target) {
                SDL_Texture* result = nullptr;
                const Uint64 deadline = SDL_GetTicks() + 5000;
                do {
                    result = target.copy(frame, SDL_COLORSPACE_BT709_LIMITED);
                    if (result || !target.deferred()) break;
                    good = SDL_RenderClear(renderer);
                    VulkanVideoInterop::lockPresent(renderer);
                    good = good && SDL_RenderPresent(renderer);
                    VulkanVideoInterop::unlockPresent(renderer);
                    SDL_Delay(1);
                } while (good && SDL_GetTicks() < deadline);
                return result;
            };
            if (presented > 0 && presented % 15 == 0) {
                for (auto* previous : wrappers)
                    good = SDL_RenderTexture(renderer, previous, nullptr, nullptr) && good;
                interop.reset();
                interop = std::make_unique<VulkanVideoInterop>(renderer, true);
            }
            SDL_Texture* texture = importReady(*interop);
            if (!texture) {
                std::fprintf(stderr, "Import failed: %s\n", interop->reason());
                good = false;
                break;
            }
            latestTexture = texture;
            wrappers.insert(texture);
            if (wrappers.size() > 3) {
                std::fprintf(stderr, "FFmpeg presentation wrappers exceeded ring capacity\n");
                good = false; break;
            }
            if (!heldTexture) {
                heldTexture = importReady(*secondInterop);
                if (!heldTexture) good = false;
            }
            // A retained presentation must stay drawable after its decoder
            // frame is released and without another source timeline handoff.
            for (int repeat = 0; repeat != 2 && good; ++repeat) {
                VulkanVideoInterop::startFrame(renderer);
                good = SDL_RenderClear(renderer) &&
                    VulkanVideoInterop::markForDraw(texture) &&
                    SDL_RenderTexture(renderer, texture, nullptr, nullptr) &&
                    VulkanVideoInterop::beginFrame(renderer);
                VulkanVideoInterop::lockPresent(renderer);
                if (good) good = SDL_RenderPresent(renderer);
                VulkanVideoInterop::unlockPresent(renderer);
                if (good) good = VulkanVideoInterop::endFrame(renderer);
            }
            ++presented;
            av_frame_unref(frame);
        }
    };
    while (good && presented < targetFrames && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream) {
            good = avcodec_send_packet(decoder, packet) >= 0;
            if (good) receive();
        }
        av_packet_unref(packet);
    }
    if (good && presented < targetFrames) {
        good = avcodec_send_packet(decoder, nullptr) >= 0;
        if (good) receive();
    }
    if (good && heldTexture && latestTexture) {
        VulkanVideoInterop::startFrame(renderer);
        const SDL_FRect inset{0, 0, float(outputW) / 4, float(outputH) / 4};
        good = SDL_RenderClear(renderer) &&
            VulkanVideoInterop::markForDraw(latestTexture) &&
            SDL_RenderTexture(renderer, latestTexture, nullptr, nullptr) &&
            VulkanVideoInterop::markForDraw(heldTexture) &&
            SDL_RenderTexture(renderer, heldTexture, nullptr, &inset);
        if (good) {
            VulkanVideoInterop::lockPresent(renderer);
            SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
            VulkanVideoInterop::unlockPresent(renderer);
            good = pixels != nullptr;
            if (pixels) {
                Uint8 minimum = 255, maximum = 0;
                for (int y = 12; y < pixels->h; y += std::max(24, pixels->h / 12))
                    for (int x = 12; x < pixels->w; x += std::max(24, pixels->w / 20)) {
                        Uint8 r = 0, g = 0, b = 0, a = 0;
                        good = good && SDL_ReadSurfacePixel(pixels, x, y,
                            &r, &g, &b, &a);
                        minimum = std::min({minimum, r, g, b});
                        maximum = std::max({maximum, r, g, b});
                    }
                good = good && maximum > minimum + 20;
                if (good && argc == 3)
                    good = SDL_SaveBMP(pixels, argv[2]);
                if (!good)
                    std::fprintf(stderr, "Readback lacks visible video pixels (%u..%u)\n",
                        minimum, maximum);
                SDL_DestroySurface(pixels);
            }
        }
        if (good) good = VulkanVideoInterop::beginFrame(renderer);
        VulkanVideoInterop::lockPresent(renderer);
        if (good) good = SDL_RenderPresent(renderer);
        VulkanVideoInterop::unlockPresent(renderer);
        if (good) good = VulkanVideoInterop::endFrame(renderer);
    }
    secondInterop.reset();
    interop.reset();
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    av_buffer_unref(&hardware);
    VulkanVideoInterop::drainRenderers();
    SDL_DestroyRenderer(renderer);
    VulkanVideoInterop::releaseRenderers();
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::fprintf(stderr, "Presented %d FFmpeg Vulkan frames twice each: %s\n",
        presented, good ? "passed" : "failed");
    return good && presented == targetFrames ? 0 : 1;
}
