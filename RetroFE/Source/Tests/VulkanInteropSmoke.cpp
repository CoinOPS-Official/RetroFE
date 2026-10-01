#include "../Video/VulkanVideoInterop.h"
#include <gst/app/gstappsink.h>
#include <gst/vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <set>

struct Stream {
    GstElement* pipeline = nullptr;
    GstElement* sink = nullptr;
    std::unique_ptr<VulkanVideoInterop> interop;
    SDL_Texture* texture = nullptr;
    std::set<SDL_Texture*> wrappers;
};

// Synthetic unsignaled producer: no decode work is allowed to hold up SDL's
// graphics queue. Also exercise the guard against importing a DPB reference.
bool delayedProducerChecks(SDL_Renderer* renderer) {
    VulkanVideoInterop interop(renderer);
    auto* element = gst_element_factory_make("identity", nullptr);
    if (!element) return false;
    interop.configureElement(element);
    auto* deviceContext = gst_element_get_context(element, GST_VULKAN_DEVICE_CONTEXT_TYPE_STR);
    auto* queueContext = gst_element_get_context(element, GST_VULKAN_QUEUE_CONTEXT_TYPE_STR);
    GstVulkanDevice* device = nullptr;
    GstVulkanQueue* queue = nullptr;
    if (deviceContext) { gst_context_get_vulkan_device(deviceContext, &device); gst_context_unref(deviceContext); }
    if (queueContext) { gst_context_get_vulkan_queue(queueContext, &queue); gst_context_unref(queueContext); }
    gst_object_unref(element);
    if (!device || !queue) {
        if (device) gst_object_unref(device);
        if (queue) gst_object_unref(queue);
        return false;
    }
    auto* raw = gst_vulkan_image_memory_alloc(device, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        64, 64, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!raw) { gst_object_unref(queue); gst_object_unref(device); return false; }
    auto* memory = reinterpret_cast<GstVulkanImageMemory*>(raw);
    memory->barrier.parent.queue = queue; // transfer the context-query reference
    memory->barrier.parent.semaphore_value = 1;
    auto* buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, raw);
    auto* caps = gst_caps_from_string("video/x-raw(memory:VulkanImage),format=NV12,width=64,height=64,framerate=30/1");
    auto* sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer); gst_caps_unref(caps);
    bool good = !interop.copy(sample) && interop.deferred();
    memory->usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    good = good && !interop.copy(sample) && !interop.deferred();
    memory->usage &= ~VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    for (int i = 0; i < 8 && good; ++i) {
        const Uint64 start = SDL_GetTicksNS();
        good = !interop.copy(sample) && interop.deferred() && SDL_RenderClear(renderer);
        VulkanVideoInterop::lockPresent(renderer);
        good = good && SDL_RenderPresent(renderer);
        VulkanVideoInterop::unlockPresent(renderer);
        if (i > 0) good = good && SDL_GetTicksNS() - start < 250000000ULL;
    }
    interop.discardFrames();
    gst_sample_unref(sample);
    gst_object_unref(device);
    if (!good) std::fprintf(stderr, "Delayed Vulkan producer/DPB contract checks failed\n");
    return good;
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 8 ||
        (std::string(argv[2]) != "h264" && std::string(argv[2]) != "h265")) {
        std::fprintf(stderr,
            "usage: retrofe_vulkan_interop_smoke <stream> <h264|h265> [frames] [presents-per-frame] [streams] [alternate] [flush]\n");
        return 2;
    }
    const int targetFrames = argc >= 4 ? std::atoi(argv[3]) : 10;
    const int presentsPerFrame = argc >= 5 ? std::atoi(argv[4]) : 1;
    const int streamCount = argc >= 6 ? std::atoi(argv[5]) : 1;
    bool alternate = false;
    bool intermediateFlush = false;
    for (int i = 6; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "alternate" && !alternate) alternate = true;
        else if (option == "flush" && !intermediateFlush) intermediateFlush = true;
        else return 2;
    }
    if (targetFrames <= 0 || presentsPerFrame < 0 || streamCount < 1 || streamCount > 2 ||
        (alternate && streamCount != 2))
        return 2;

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = SDL_CreateWindow("Vulkan Video smoke", 256, 144,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) return 1;
    SDL_Renderer* renderer = VulkanVideoInterop::createRenderer(window);
    if (!renderer) return 1;


    std::string source = argv[1];
    for (char& c : source) if (c == '\\') c = '/';
    const std::string codec = argv[2];
    const std::string pipelineText = std::string("filesrc location=\"") + source +
        "\" ! " + codec + "parse ! identity sleep-time=33333 ! vulkan" + codec + "dec ! "
        "appsink name=output sync=true max-buffers=1 drop=true "
        "caps=\"video/x-raw(memory:VulkanImage),format=NV12\"";
    std::vector<Stream> streams(streamCount);
    bool good = delayedProducerChecks(renderer);
    int frames = 0;
    for (auto& stream : streams) {
        GError* parseError = nullptr;
        stream.pipeline = gst_parse_launch(pipelineText.c_str(), &parseError);
        if (parseError) {
            std::fprintf(stderr, "Pipeline: %s\n", parseError->message);
            g_error_free(parseError);
        }
        if (!stream.pipeline) { good = false; break; }
        stream.sink = gst_bin_get_by_name(GST_BIN(stream.pipeline), "output");
        if (!stream.sink) { good = false; break; }
        stream.interop = std::make_unique<VulkanVideoInterop>(renderer);
        stream.interop->configure(stream.pipeline);
        stream.interop->wrapSink(stream.sink);
        if (gst_element_set_state(stream.pipeline, GST_STATE_PLAYING) ==
                GST_STATE_CHANGE_FAILURE) { good = false; break; }
    }

    while (good && frames < targetFrames) {
        for (auto& stream : streams) {
            if (frames > 0 && frames % 15 == 0) {
                // Leave old wrappers in SDL's batch when the lease changes.
                // The next GPU copy must submit these reads before reuse.
                for (auto* texture : stream.wrappers)
                    good = SDL_RenderTexture(renderer, texture, nullptr, nullptr) && good;
                stream.texture = nullptr;
                stream.interop.reset();
                stream.interop = std::make_unique<VulkanVideoInterop>(renderer);
                stream.interop->configure(stream.pipeline);
                stream.interop->wrapSink(stream.sink);
            }
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(stream.sink),
                5 * GST_SECOND);
            if (!sample) {
                std::fprintf(stderr, "Stream %td produced no sample (EOS=%d)\n",
                    &stream - streams.data(), gst_app_sink_is_eos(GST_APP_SINK(stream.sink)));
                good = false;
                break;
            }
            if (presentsPerFrame == 0) {
                gst_sample_unref(sample);
                continue;
            }
            SDL_Texture* imported = nullptr;
            const Uint64 deadline = SDL_GetTicks() + 5000;
            do {
                imported = stream.interop->copy(sample);
                if (imported || !stream.interop->deferred()) break;
                good = SDL_RenderClear(renderer);
                VulkanVideoInterop::lockPresent(renderer);
                good = good && SDL_RenderPresent(renderer);
                VulkanVideoInterop::unlockPresent(renderer);
                SDL_Delay(1);
            } while (good && SDL_GetTicks() < deadline);
            stream.texture = imported;
            gst_sample_unref(sample);
            if (!stream.texture) {
                std::fprintf(stderr, "Stream %td import: %s\n",
                    &stream - streams.data(), stream.interop->reason());
                good = false;
                break;
            }
            stream.wrappers.insert(stream.texture);
            if (stream.wrappers.size() > 3) {
                std::fprintf(stderr, "Presentation wrappers grew beyond the three-slot ring\n");
                good = false; break;
            }
        }
        for (int present = 0; present < presentsPerFrame && good; ++present) {
            VulkanVideoInterop::startFrame(renderer);
            good = SDL_RenderClear(renderer);
            for (int i = 0; i < streamCount && good; ++i) {
                if (alternate && i != frames % streamCount) continue;
                const SDL_FRect dst = {128.0f * i, 0.0f, 128.0f, 144.0f};
                good = VulkanVideoInterop::markForDraw(streams[i].texture) &&
                    SDL_RenderTexture(renderer, streams[i].texture, nullptr, &dst);
            }
            if (good && intermediateFlush) {
                VulkanVideoInterop::lockPresent(renderer);
                good = SDL_FlushRenderer(renderer);
                VulkanVideoInterop::unlockPresent(renderer);
            }
            if (good) good = VulkanVideoInterop::beginFrame(renderer);
            VulkanVideoInterop::lockPresent(renderer);
            if (good) good = SDL_RenderPresent(renderer);
            VulkanVideoInterop::unlockPresent(renderer);
            if (good) good = VulkanVideoInterop::endFrame(renderer);
        }
        ++frames;
    }
    if (!good) std::fprintf(stderr, "Vulkan frame %d failed: %s\n",
        frames, SDL_GetError());
    for (auto& stream : streams) stream.interop.reset();
    for (auto& stream : streams)
        if (stream.pipeline) gst_element_set_state(stream.pipeline, GST_STATE_NULL);
    VulkanVideoInterop::drainRenderers();
    for (auto& stream : streams) {
        if (stream.sink) gst_object_unref(stream.sink);
        if (stream.pipeline) gst_object_unref(stream.pipeline);
    }
    SDL_DestroyRenderer(renderer);
    VulkanVideoInterop::releaseRenderers();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (good) std::fprintf(stderr, "Presented %d Vulkan %s frames, %d streams, %d presents each%s%s\n",
        frames, codec.c_str(), streamCount, presentsPerFrame,
        alternate ? ", alternating visibility" : "",
        intermediateFlush ? ", intermediate SDL flush" : "");
    return good && frames == targetFrames ? 0 : 1;
}
