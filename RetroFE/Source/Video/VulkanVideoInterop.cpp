#include "VulkanVideoInterop.h"
#include "../Utility/Log.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL_vulkan.h>
#include <gst/vulkan/vulkan.h>
#ifdef RETROFE_HAVE_FFMPEG
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/version.h>
}
#endif
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct PresentationSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    SDL_Texture* texture = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkSemaphore completion = VK_NULL_HANDLE;
    uint64_t value = 0;
    bool initialized = false;
    std::shared_ptr<void> owner;
};
struct PresentationRing {
    int width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    SDL_Colorspace color = SDL_COLORSPACE_UNKNOWN;
    std::array<PresentationSlot, 3> slots{};
    size_t next = 0;
    bool ready = false;
    bool leased = false;
};

struct SharedDevice {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    GstVulkanInstance* instance = nullptr;
    GstVulkanDevice* device = nullptr;
    GstVulkanQueue* graphics = nullptr;
    GstVulkanQueue* video = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    uint32_t graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
    std::vector<std::unique_ptr<PresentationRing>> presentationRings;
    void collect(PresentationRing& ring) {
        for (auto& slot : ring.slots) if (slot.owner) {
            uint64_t completed = 0;
            if (vkGetSemaphoreCounterValue(device->device, slot.completion,
                    &completed) == VK_SUCCESS && completed >= slot.value)
                slot.owner.reset();
        }
    }
    void collect() {
        for (auto& ring : presentationRings) collect(*ring);
    }
    void clearPresentation() {
        if (presentationRings.empty()) return;
        // Renderer teardown only. SDL's Vulkan destructor implicitly submits
        // queued draws and waits for graphics queue completion.
        gst_vulkan_queue_submit_lock(graphics);
        vkQueueWaitIdle(graphics->queue);
        for (auto& ring : presentationRings) for (auto& slot : ring->slots) {
            if (slot.texture) SDL_DestroyTexture(slot.texture);
            if (slot.pool) vkDestroyCommandPool(device->device, slot.pool, nullptr);
            if (slot.completion) vkDestroySemaphore(device->device, slot.completion, nullptr);
            if (slot.image) vkDestroyImage(device->device, slot.image, nullptr);
            if (slot.memory) vkFreeMemory(device->device, slot.memory, nullptr);
            slot.owner.reset();
        }
        gst_vulkan_queue_submit_unlock(graphics);
        presentationRings.clear();
    }
#ifdef RETROFE_HAVE_FFMPEG
    VkPhysicalDeviceFeatures2 ffmpegFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features ffmpegFeatures11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features ffmpegFeatures12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features ffmpegFeatures13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    std::vector<const char*> ffmpegExtensions;
    // FFmpeg's virtual graphics queue 0 maps to native queue 1. SDL owns native
    // queue 0 exclusively, including implicit submits in texture/target APIs.
    std::array<std::array<GstVulkanQueue*, 2>, 64> ffmpegQueues{};

#endif

    ~SharedDevice() {
        if (device && device->device) vkDeviceWaitIdle(device->device);
#ifdef RETROFE_HAVE_FFMPEG
        for (auto& family : ffmpegQueues) for (auto* queue : family)
            if (queue) gst_object_unref(queue);
#endif
        if (graphics) gst_object_unref(graphics);
        if (video) gst_object_unref(video);
        if (device) gst_object_unref(device);
        if (surface && instance) vkDestroySurfaceKHR(instance->instance, surface, nullptr);
        if (instance) gst_object_unref(instance);
    }
};

std::vector<std::unique_ptr<SharedDevice>>& devices() {
    static std::vector<std::unique_ptr<SharedDevice>> list;
    return list;
}

std::vector<VulkanVideoInterop*>& interops() {
    static std::vector<VulkanVideoInterop*> list;
    return list;
}

SharedDevice* shared(SDL_Renderer* renderer) {
    for (auto& item : devices())
        if (item->renderer == renderer) return item.get();
    return nullptr;
}

bool setContext(GstElement* element, SharedDevice* sharedDevice) {
    if (!element || !sharedDevice) return false;
    GstContext* instanceContext = gst_context_new(
        GST_VULKAN_INSTANCE_CONTEXT_TYPE_STR, TRUE);
    GstContext* deviceContext = gst_context_new(
        GST_VULKAN_DEVICE_CONTEXT_TYPE_STR, TRUE);
    GstContext* queueContext = gst_context_new(
        GST_VULKAN_QUEUE_CONTEXT_TYPE_STR, TRUE);
    if (!instanceContext || !deviceContext || !queueContext) {
        if (instanceContext) gst_context_unref(instanceContext);
        if (deviceContext) gst_context_unref(deviceContext);
        if (queueContext) gst_context_unref(queueContext);
        return false;
    }
    gst_context_set_vulkan_instance(instanceContext, sharedDevice->instance);
    gst_context_set_vulkan_device(deviceContext, sharedDevice->device);
    gst_context_set_vulkan_queue(queueContext,
        sharedDevice->video ? sharedDevice->video : sharedDevice->graphics);
    gst_element_set_context(element, instanceContext);
    gst_element_set_context(element, deviceContext);
    gst_element_set_context(element, queueContext);
    gst_context_unref(instanceContext);
    gst_context_unref(deviceContext);
    gst_context_unref(queueContext);
    return true;
}

GstPadProbeReturn answerContextQuery(GstPad*, GstPadProbeInfo* info,
    gpointer userData) {
    auto* sharedDevice = static_cast<SharedDevice*>(userData);
    GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
    if (!query || GST_QUERY_TYPE(query) != GST_QUERY_CONTEXT)
        return GST_PAD_PROBE_OK;
    const gchar* type = nullptr;
    gst_query_parse_context_type(query, &type);
    if (!type) return GST_PAD_PROBE_OK;
    GstContext* context = nullptr;
    if (g_strcmp0(type, GST_VULKAN_INSTANCE_CONTEXT_TYPE_STR) == 0) {
        context = gst_context_new(type, TRUE);
        gst_context_set_vulkan_instance(context, sharedDevice->instance);
    } else if (g_strcmp0(type, GST_VULKAN_DEVICE_CONTEXT_TYPE_STR) == 0) {
        context = gst_context_new(type, TRUE);
        gst_context_set_vulkan_device(context, sharedDevice->device);
    } else if (g_strcmp0(type, GST_VULKAN_QUEUE_CONTEXT_TYPE_STR) == 0) {
        context = gst_context_new(type, TRUE);
        gst_context_set_vulkan_queue(context,
            sharedDevice->video ? sharedDevice->video : sharedDevice->graphics);
    }
    if (!context) return GST_PAD_PROBE_OK;
    gst_query_set_context(query, context);
    gst_context_unref(context);
    return GST_PAD_PROBE_HANDLED;
}
}

SDL_Renderer* VulkanVideoInterop::createRenderer(SDL_Window* window) {
    if (!window) return nullptr;
    gst_init(nullptr, nullptr);
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        LOG_WARNING("VulkanVideoInterop", std::string("Vulkan loader unavailable: ") + SDL_GetError());
        return nullptr;
    }
    auto candidate = std::make_unique<SharedDevice>();
    candidate->window = window;
    candidate->instance = gst_vulkan_instance_new();
    GError* error = nullptr;
    if (!candidate->instance || !gst_vulkan_instance_open(candidate->instance, &error)) {
        LOG_WARNING("VulkanVideoInterop", std::string("Vulkan instance unavailable: ") +
            (error ? error->message : "unknown error"));
        if (error) g_error_free(error);
        return nullptr;
    }
    if (!SDL_Vulkan_CreateSurface(window, candidate->instance->instance, nullptr,
            &candidate->surface)) {
        LOG_WARNING("VulkanVideoInterop", std::string("Vulkan surface unavailable: ") + SDL_GetError());
        return nullptr;
    }

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(candidate->instance->instance, &count, nullptr) != VK_SUCCESS || !count)
        return nullptr;
    std::vector<VkPhysicalDevice> physical(count);
    if (vkEnumeratePhysicalDevices(candidate->instance->instance, &count, physical.data()) != VK_SUCCESS)
        return nullptr;
    struct QueueChoice {
        uint32_t deviceIndex;
        uint32_t family;
        unsigned codecs;
    };
    std::vector<QueueChoice> choices;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical[i], nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount && vkEnumerateDeviceExtensionProperties(physical[i], nullptr,
                &extensionCount, extensions.data()) != VK_SUCCESS)
            continue;
        auto hasExtension = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(),
                [name](const VkExtensionProperties& ext) {
                    return std::strcmp(ext.extensionName, name) == 0;
                });
        };
        const bool videoDecode = hasExtension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) &&
            hasExtension(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties2(physical[i], &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties2> families(familyCount);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(familyCount);
        for (uint32_t family = 0; family < familyCount; ++family) {
            families[family].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            if (videoDecode) {
                families[family].pNext = &video[family];
                video[family].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            }
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(physical[i], &familyCount, families.data());
        for (uint32_t family = 0; family < familyCount; ++family) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical[i], family,
                candidate->surface, &present);
            const VkQueueFlags flags = families[family].queueFamilyProperties.queueFlags;
            if (!present || !(flags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            unsigned codecs = 0;
            if (videoDecode && (flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)) {
                const auto operations = video[family].videoCodecOperations;
                if ((operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) &&
                    hasExtension(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME)) codecs |= 1;
                if ((operations & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) &&
                    hasExtension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME)) codecs |= 2;
            }
            choices.push_back({i, family, codecs});
        }
    }
    // Keep decode and SDL graphics submissions on one queue family. Prefer an
    // adapter that can decode both codecs, then one codec, then graphics only.
    std::stable_sort(choices.begin(), choices.end(),
        [](const QueueChoice& a, const QueueChoice& b) {
            const unsigned aCount = unsigned((a.codecs & 1) != 0) + unsigned((a.codecs & 2) != 0);
            const unsigned bCount = unsigned((b.codecs & 1) != 0) + unsigned((b.codecs & 2) != 0);
            return aCount > bCount;
        });
    for (const QueueChoice& choice : choices) {
        candidate->device = gst_vulkan_device_new_with_index(candidate->instance,
            choice.deviceIndex);
        error = nullptr;
        if (candidate->device && gst_vulkan_device_open(candidate->device, &error)) {
            candidate->graphics = gst_vulkan_device_get_queue(candidate->device,
                choice.family, 0);
            if (candidate->graphics) {
                candidate->video = gst_vulkan_device_get_queue(candidate->device,
                    choice.family, 1);
                candidate->graphicsFamily = choice.family;
                candidate->physical = physical[choice.deviceIndex];
#ifdef RETROFE_HAVE_FFMPEG
                candidate->ffmpegFeatures.pNext = &candidate->ffmpegFeatures11;
                candidate->ffmpegFeatures11.pNext = &candidate->ffmpegFeatures12;
                candidate->ffmpegFeatures12.pNext = &candidate->ffmpegFeatures13;
                vkGetPhysicalDeviceFeatures2(candidate->physical,
                    &candidate->ffmpegFeatures);
                for (const char* extension : {
                         VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                         VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
                         VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
                         VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
                         VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
                         VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
                         VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
                         VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME}) {
                    if (gst_vulkan_device_is_extension_enabled(candidate->device,
                            extension))
                        candidate->ffmpegExtensions.push_back(extension);
                }
#endif
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate->physical, &properties);
                LOG_INFO("VulkanVideoInterop", std::string("Selected ") + properties.deviceName +
                    " (separate video queue=" +
                    (candidate->video ? "yes" : "no") +
                    ", combined graphics/decode H.264=" +
                    ((choice.codecs & 1) ? "yes" : "no") + ", H.265=" +
                    ((choice.codecs & 2) ? "yes" : "no") + ")");
                break;
            }
        }
        if (error) g_error_free(error);
        if (candidate->device) gst_object_unref(candidate->device);
        candidate->device = nullptr;
    }
    if (!candidate->graphics) {
        LOG_WARNING("VulkanVideoInterop", "No shared present/graphics Vulkan Video device");
        return nullptr;
    }

    const SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) return nullptr;
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_INSTANCE_POINTER,
        candidate->instance->instance);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_SURFACE_NUMBER,
        (Sint64)candidate->surface);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_PHYSICAL_DEVICE_POINTER,
        candidate->physical);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_DEVICE_POINTER,
        candidate->device->device);
    SDL_SetNumberProperty(props,
        SDL_PROP_RENDERER_CREATE_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER,
        candidate->graphicsFamily);
    SDL_SetNumberProperty(props,
        SDL_PROP_RENDERER_CREATE_VULKAN_PRESENT_QUEUE_FAMILY_INDEX_NUMBER,
        candidate->graphicsFamily);
    candidate->renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    if (!candidate->renderer) {
        LOG_WARNING("VulkanVideoInterop", std::string("Shared Vulkan renderer unavailable: ") + SDL_GetError());
        return nullptr;
    }
    SDL_Renderer* renderer = candidate->renderer;
    devices().push_back(std::move(candidate));
    LOG_INFO("VulkanVideoInterop", "SDL and GStreamer share one Vulkan Video device");
    return renderer;
}

bool VulkanVideoInterop::hasSharedDevice(SDL_Renderer* renderer) {
    return shared(renderer) != nullptr;
}
bool VulkanVideoInterop::supportsVideo(SDL_Renderer* renderer) {
    const SharedDevice* device = shared(renderer);
    return device && device->video;
}
bool VulkanVideoInterop::sharedDevice(SDL_Renderer* renderer, DeviceInfo& info) {
    SharedDevice* device = shared(renderer);
    if (!device) return false;
    info.instance = device->instance->instance;
    info.physical = device->physical;
    info.device = device->device->device;
    info.graphicsQueue = device->graphics->queue;
    info.videoQueue = device->video ? device->video->queue : VK_NULL_HANDLE;
    info.graphicsFamily = device->graphicsFamily;
    return true;
}

#ifdef RETROFE_HAVE_FFMPEG
namespace {
SharedDevice* sharedNativeDevice(VkDevice native) {
    for (auto& device : devices())
        if (device->device->device == native) return device.get();
    return nullptr;
}
uint32_t ffmpegQueueIndex(VkDevice native, uint32_t family, uint32_t index) {
    auto* device = sharedNativeDevice(native);
    return device && family == device->graphicsFamily ? index + 1 : index;
}
VKAPI_ATTR void VKAPI_CALL ffmpegGetDeviceQueue(VkDevice device, uint32_t family,
    uint32_t index, VkQueue* queue) {
    vkGetDeviceQueue(device, family, ffmpegQueueIndex(device, family, index), queue);
}
VKAPI_ATTR void VKAPI_CALL ffmpegGetDeviceQueue2(VkDevice device,
    const VkDeviceQueueInfo2* info, VkQueue* queue) {
    VkDeviceQueueInfo2 mapped = *info;
    mapped.queueIndex = ffmpegQueueIndex(device, mapped.queueFamilyIndex, mapped.queueIndex);
    vkGetDeviceQueue2(device, &mapped, queue);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffmpegGetDeviceProcAddr(VkDevice device,
    const char* name) {
    if (std::strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(ffmpegGetDeviceQueue);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(ffmpegGetDeviceQueue2);
    return vkGetDeviceProcAddr(device, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffmpegGetInstanceProcAddr(VkInstance instance,
    const char* name) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(ffmpegGetDeviceProcAddr);
    if (std::strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(ffmpegGetDeviceQueue);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(ffmpegGetDeviceQueue2);
    return vkGetInstanceProcAddr(instance, name);
}
void lockFFmpegQueue(AVHWDeviceContext* ctx, uint32_t family, uint32_t index) {
    auto* device = static_cast<SharedDevice*>(ctx->user_opaque);
    if (!device) return;
    if (family < device->ffmpegQueues.size() && index < 2 && device->ffmpegQueues[family][index])
        gst_vulkan_queue_submit_lock(device->ffmpegQueues[family][index]);
}
void unlockFFmpegQueue(AVHWDeviceContext* ctx, uint32_t family, uint32_t index) {
    auto* device = static_cast<SharedDevice*>(ctx->user_opaque);
    if (!device) return;
    if (family < device->ffmpegQueues.size() && index < 2 && device->ffmpegQueues[family][index])
        gst_vulkan_queue_submit_unlock(device->ffmpegQueues[family][index]);
}
}

AVBufferRef* VulkanVideoInterop::createFFmpegDevice(SDL_Renderer* renderer) {
    SharedDevice* device = shared(renderer);
    if (!device || !device->physical || !device->graphics || !device->video) return nullptr;
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!ref) return nullptr;
    auto* context = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* vk = static_cast<AVVulkanDeviceContext*>(context->hwctx);
    context->user_opaque = device;
    vk->get_proc_addr = ffmpegGetInstanceProcAddr;
    vk->inst = device->instance->instance;
    vk->phys_dev = device->physical;
    vk->act_dev = device->device->device;
    vk->device_features = device->ffmpegFeatures;
    vk->enabled_dev_extensions = device->ffmpegExtensions.data();
    vk->nb_enabled_dev_extensions = int(device->ffmpegExtensions.size());
    vk->lock_queue = lockFFmpegQueue;
    vk->unlock_queue = unlockFFmpegQueue;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(device->physical, &familyCount, nullptr);
    if (familyCount == 0 || familyCount > 64) {
        av_buffer_unref(&ref);
        return nullptr;
    }
    std::vector<VkQueueFamilyProperties2> families(familyCount);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video(familyCount);
    for (uint32_t i = 0; i < familyCount; ++i) {
        families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        families[i].pNext = &video[i];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(device->physical, &familyCount,
        families.data());
    std::array<uint32_t, 64> enabledCounts{};
    gst_vulkan_device_foreach_queue(device->device,
        [](GstVulkanDevice*, GstVulkanQueue* queue, gpointer data) -> gboolean {
            auto& counts = *static_cast<std::array<uint32_t, 64>*>(data);
            if (queue->family < counts.size())
                counts[queue->family] = std::max(counts[queue->family],
                    queue->index + 1);
            return TRUE;
        }, &enabledCounts);
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (!enabledCounts[i]) continue;
        auto& entry = vk->qf[vk->nb_qf++];
        entry.idx = int(i);
        entry.num = i == device->graphicsFamily ? 1 : int(enabledCounts[i]);
        if (entry.num > 2) { av_buffer_unref(&ref); return nullptr; }
        for (int index = 0; index < entry.num; ++index) {
            if (!device->ffmpegQueues[i][index])
                device->ffmpegQueues[i][index] = gst_vulkan_device_get_queue(
                    device->device, i, i == device->graphicsFamily ? index + 1 : index);
            if (!device->ffmpegQueues[i][index]) { av_buffer_unref(&ref); return nullptr; }
        }
        entry.flags = static_cast<VkQueueFlagBits>(
            families[i].queueFamilyProperties.queueFlags);
        entry.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
            video[i].videoCodecOperations);
    }
    const int result = av_hwdevice_ctx_init(ref);
    if (result < 0) {
        char reason[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, reason, sizeof(reason));
        LOG_WARNING("VulkanVideoInterop", std::string("FFmpeg shared Vulkan device failed: ") + reason);
        av_buffer_unref(&ref);
        return nullptr;
    }
    LOG_INFO("VulkanVideoInterop", "FFmpeg decoder shares the SDL Vulkan device");
    return ref;
}
#endif

struct VulkanVideoInterop::Impl {
    explicit Impl(SDL_Renderer* r, bool ff) : renderer(r), context(shared(r)), ffmpeg(ff) {
        if (context && !context->video && !ffmpeg)
            error = "GPU has no second graphics queue for Vulkan video interop";
    }
    SDL_Renderer* renderer = nullptr;
    SharedDevice* context = nullptr;
    bool ffmpeg = false;
    GstElement* pipeline = nullptr;
    GstPad* probePad = nullptr;
    gulong probeId = 0;
    std::string error = "SDL renderer has no shared Vulkan Video device";
    bool deferred = false;
    bool failureLogged = false;
    SDL_Texture* current = nullptr;

    using Slot = PresentationSlot;
    using Ring = PresentationRing;
    std::vector<Ring*> rings; // leases of renderer-owned presentation resources

    bool fail(const char* reason) {
        error = reason;
        if (!failureLogged) { LOG_ERROR("VulkanVideoInterop", error); failureLogged = true; }
        return false;
    }
    void removeProbe() {
        if (probePad) {
            if (probeId) gst_pad_remove_probe(probePad, probeId);
            gst_object_unref(probePad);
        }
        probePad = nullptr;
        probeId = 0;
    }
    void collect() { if (context) context->collect(); }
    void clear() {
        collect();
        for (auto* ring : rings) ring->leased = false;
        rings.clear();
        current = nullptr;
    }
    ~Impl() { clear(); }

    Ring* allocate(int w, int h, VkFormat format, SDL_PixelFormat pixel, SDL_Colorspace color) {
        for (auto& ring : rings)
            if (ring->width == w && ring->height == h && ring->format == format && ring->color == color)
                return ring->ready ? ring : nullptr;
        if (rings.size() >= 4) {
            fail("Vulkan presentation allocation cache is full; software fallback required");
            return nullptr;
        }
        for (auto& ring : context->presentationRings) {
            if (!ring->leased && ring->ready && ring->width == w && ring->height == h &&
                    ring->format == format && ring->color == color) {
                ring->leased = true;
                rings.push_back(ring.get());
                return ring.get();
            }
        }
        // Renderer-wide cap also bounds SDL's persistent YUV pipeline cache,
        // including repeated construction/destruction of video objects.
        if (context->presentationRings.size() >= 32) {
            fail("Renderer Vulkan presentation pool is full; software fallback required");
            return nullptr;
        }
        auto ring = std::make_unique<Ring>();
        ring->width = w; ring->height = h; ring->format = format; ring->color = color;
        ring->leased = true;
        Ring* result = ring.get();
        context->presentationRings.push_back(std::move(ring));
        rings.push_back(result);
        const VkDevice device = context->device->device;
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(context->physical, &memoryProperties);
        for (auto& slot : result->slots) {
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = format;
            image.extent = {uint32_t((w + 1) & ~1), uint32_t((h + 1) & ~1), 1};
            image.mipLevels = image.arrayLayers = 1;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(device, &image, nullptr, &slot.image) != VK_SUCCESS) {
                fail("Cannot create Vulkan presentation image"); return nullptr;
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, slot.image, &requirements);
            uint32_t type = memoryProperties.memoryTypeCount;
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = i; break;
                }
            if (type == memoryProperties.memoryTypeCount) { fail("No device-local video memory"); return nullptr; }
            VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            memory.allocationSize = requirements.size;
            memory.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &memory, nullptr, &slot.memory) != VK_SUCCESS ||
                vkBindImageMemory(device, slot.image, slot.memory, 0) != VK_SUCCESS) {
                fail("Cannot allocate Vulkan presentation memory"); return nullptr;
            }
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = context->graphicsFamily;
            VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commands.commandBufferCount = 1;
            VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            semaphore.pNext = &typeInfo;
            if (vkCreateCommandPool(device, &pool, nullptr, &slot.pool) != VK_SUCCESS) {
                fail("Cannot create presentation copy pool"); return nullptr;
            }
            commands.commandPool = slot.pool;
            if (vkAllocateCommandBuffers(device, &commands, &slot.commands) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphore, nullptr, &slot.completion) != VK_SUCCESS) {
                fail("Cannot create presentation copy synchronization"); return nullptr;
            }
            const auto props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, pixel);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, color);
            // External SDL images start tracked as SHADER_READ_ONLY_OPTIMAL.
            // The first copy establishes that layout before any SDL draw.
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER, (Sint64)slot.image);
            slot.texture = SDL_CreateTextureWithProperties(renderer, props);
            SDL_DestroyProperties(props);
            if (!slot.texture) { fail(SDL_GetError()); return nullptr; }
        }
        result->ready = true;
        return result;
    }

    SDL_Texture* transfer(VkImage source, VkImageLayout layout, VkSemaphore producer,
        uint64_t value, VkFormat format, SDL_PixelFormat pixel, SDL_Colorspace color,
        int x, int y, int w, int h, std::shared_ptr<void> owner) {
        uint64_t completed = 0;
        if (vkGetSemaphoreCounterValue(context->device->device, producer, &completed) != VK_SUCCESS) {
            fail("Cannot poll Vulkan producer timeline"); return nullptr;
        }
        // Even a GPU-side wait on an unfinished decode would hold up SDL's
        // graphics queue. Only submit ready frames; keep the last picture.
        if (completed < value) { deferred = true; return nullptr; }
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
            fail("Vulkan producer image has no completed content layout"); return nullptr;
        }
        Ring* ring = allocate(w, h, format, pixel, color);
        if (!ring) return nullptr;
        // Imports need only reclaim their own three slots. Retire idle leases
        // once at the renderer frame boundary, rather than scanning every
        // video's allocations for every new decoded frame.
        context->collect(*ring);
        Slot* selected = nullptr;
        for (size_t n = 0; n < ring->slots.size(); ++n) {
            const size_t index = (ring->next + n) % ring->slots.size();
            auto& slot = ring->slots[index];
            if (!slot.owner && slot.texture && slot.texture != current) {
                selected = &slot; ring->next = (index + 1) % ring->slots.size(); break;
            }
        }
        if (!selected) { deferred = true; return nullptr; }
        auto& slot = *selected;
        // An old wrapper can still occur in SDL's unsubmitted batch, including
        // after another interop relinquishes its lease. Submit those reads
        // before overwriting its image on the same graphics queue.
        if (slot.initialized && !SDL_FlushRenderer(context->renderer)) {
            fail("Cannot submit previous SDL reads before Vulkan copy"); return nullptr;
        }
        const VkDevice device = context->device->device;
        if (vkResetCommandPool(device, slot.pool, 0) != VK_SUCCESS) {
            fail("Cannot reset Vulkan copy pool"); return nullptr;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(slot.commands, &begin) != VK_SUCCESS) {
            fail("Cannot begin Vulkan copy"); return nullptr;
        }
        std::array<VkImageMemoryBarrier2, 2> barriers{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        // Producer semaphore supplies visibility; a decode stage is not valid
        // on a graphics-only queue. Restore source layout before releasing it.
        barriers[0].image = source;
        barriers[0].oldLayout = layout;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[1].image = slot.image;
        barriers[1].oldLayout = slot.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[1].srcAccessMask = slot.initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
        barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = uint32_t(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(slot.commands, &dependency);
        const unsigned planes = pixel == SDL_PIXELFORMAT_IYUV ? 3 : 2;
        std::array<VkImageCopy, 3> regions{};
        for (unsigned plane = 0; plane < planes; ++plane) {
            const uint32_t divisor = plane ? 2 : 1;
            const auto aspect = VkImageAspectFlags(VK_IMAGE_ASPECT_PLANE_0_BIT << plane);
            regions[plane].srcSubresource = {aspect, 0, 0, 1};
            regions[plane].dstSubresource = {aspect, 0, 0, 1};
            regions[plane].srcOffset = {x / int(divisor), y / int(divisor), 0};
            regions[plane].extent = {uint32_t(w + divisor - 1) / divisor,
                uint32_t(h + divisor - 1) / divisor, 1};
        }
        vkCmdCopyImage(slot.commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, planes, regions.data());
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = layout;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barriers[0].dstAccessMask = 0;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vkCmdPipelineBarrier2(slot.commands, &dependency);
        if (vkEndCommandBuffer(slot.commands) != VK_SUCCESS) {
            fail("Cannot finish Vulkan copy"); return nullptr;
        }
        const std::array<VkSemaphore, 2> signals{producer, slot.completion};
        const std::array<uint64_t, 2> values{value + 1, slot.value + 1};
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &value;
        timeline.signalSemaphoreValueCount = uint32_t(signals.size());
        timeline.pSignalSemaphoreValues = values.data();
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &producer;
        submit.pWaitDstStageMask = &stage;
        submit.signalSemaphoreCount = uint32_t(signals.size());
        submit.pSignalSemaphores = signals.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.commands;
        gst_vulkan_queue_submit_lock(context->graphics);
        const VkResult status = vkQueueSubmit(context->graphics->queue, 1, &submit, VK_NULL_HANDLE);
        gst_vulkan_queue_submit_unlock(context->graphics);
        if (status != VK_SUCCESS) { fail("Cannot submit Vulkan presentation copy"); return nullptr; }
        slot.value = values[1];
        slot.owner = std::move(owner);
        slot.initialized = true;
        current = slot.texture;
        error.clear(); failureLogged = false;
        return current;
    }

    SDL_Texture* copy(GstSample* sample) {
        deferred = false;
        if (!context || !sample) return nullptr;
        GstVideoInfo info{};
        auto* buffer = gst_sample_get_buffer(sample);
        if (!buffer || !gst_sample_get_caps(sample) ||
            !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
            gst_buffer_n_memory(buffer) != 1 || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
            fail("Expected a single NV12 Vulkan image"); return nullptr;
        }
        auto* raw = gst_buffer_peek_memory(buffer, 0);
        if (!gst_is_vulkan_image_memory(raw)) { fail("Decoder output is not VulkanImage memory"); return nullptr; }
        auto* memory = reinterpret_cast<GstVulkanImageMemory*>(raw);
        if (memory->device != context->device ||
            memory->create_info.format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
            !(memory->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            (memory->usage & VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR) ||
            !memory->barrier.parent.semaphore || !context->video ||
            context->video->family != context->graphicsFamily ||
            (memory->create_info.sharingMode != VK_SHARING_MODE_CONCURRENT &&
             memory->barrier.parent.queue && memory->barrier.parent.queue->family != context->graphicsFamily)) {
            const std::string reason = "Vulkan output must be a separate transferable image on the shared graphics family: usage=" +
                std::to_string(memory->usage) + ", sharedDevice=" +
                std::to_string(memory->device == context->device) + ", producerFamily=" +
                std::to_string(memory->barrier.parent.queue ? memory->barrier.parent.queue->family : UINT32_MAX) +
                ", graphicsFamily=" + std::to_string(context->graphicsFamily);
            fail(reason.c_str()); return nullptr;
        }
        int x = 0, y = 0, w = GST_VIDEO_INFO_WIDTH(&info), h = GST_VIDEO_INFO_HEIGHT(&info);
        if (auto* crop = gst_buffer_get_video_crop_meta(buffer)) {
            x = int(crop->x); y = int(crop->y); w = int(crop->width); h = int(crop->height);
        }
        if (x < 0 || y < 0 || (x & 1) || (y & 1) || w <= 0 || h <= 0 ||
            uint64_t(x) + w > memory->create_info.extent.width ||
            uint64_t(y) + h > memory->create_info.extent.height) {
            fail("Invalid Vulkan NV12 crop"); return nullptr;
        }
        const bool full = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
        SDL_Colorspace color = full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
        if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601)
            color = full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
        else if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
            color = full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
        auto owner = std::shared_ptr<void>(gst_sample_ref(sample), [](void* sample) { gst_sample_unref(static_cast<GstSample*>(sample)); });
        auto* texture = transfer(memory->image, memory->barrier.image_layout,
            memory->barrier.parent.semaphore, memory->barrier.parent.semaphore_value,
            memory->create_info.format, SDL_PIXELFORMAT_NV12, color, x, y, w, h, std::move(owner));
        if (texture) {
            ++memory->barrier.parent.semaphore_value;
            auto* previous = memory->barrier.parent.queue;
            memory->barrier.parent.queue = GST_VULKAN_QUEUE(gst_object_ref(context->graphics));
            if (previous) gst_object_unref(previous);
            memory->barrier.parent.pipeline_stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            memory->barrier.parent.access_flags = VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        return texture;
    }
#ifdef RETROFE_HAVE_FFMPEG
    SDL_Texture* copy(AVFrame* decoded, SDL_Colorspace color) {
        deferred = false;
        if (!context || !decoded || decoded->format != AV_PIX_FMT_VULKAN || !decoded->hw_frames_ctx || !decoded->data[0]) return nullptr;
        auto* frames = reinterpret_cast<AVHWFramesContext*>(decoded->hw_frames_ctx->data);
        auto* device = reinterpret_cast<AVVulkanDeviceContext*>(frames->device_ctx->hwctx);
        auto* vkFrames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        auto* vk = reinterpret_cast<AVVkFrame*>(decoded->data[0]);
        SDL_PixelFormat pixel = SDL_PIXELFORMAT_UNKNOWN;
        switch (vkFrames->format[0]) {
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: pixel = SDL_PIXELFORMAT_NV12; break;
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: pixel = SDL_PIXELFORMAT_IYUV; break;
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: pixel = SDL_PIXELFORMAT_P010; break;
        default: fail("Unsupported FFmpeg Vulkan output format"); return nullptr;
        }
        if (device->act_dev != context->device->device || !vk->img[0] || vk->img[1] || !vk->sem[0] ||
            !(vkFrames->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || vk->tiling != VK_IMAGE_TILING_OPTIMAL ||
            (vk->queue_family[0] != VK_QUEUE_FAMILY_IGNORED && vk->queue_family[0] != context->graphicsFamily)) {
            fail("FFmpeg Vulkan output cannot be copied on the shared graphics queue"); return nullptr;
        }
        const int x = int(decoded->crop_left), y = int(decoded->crop_top);
        const int w = decoded->width - x - int(decoded->crop_right);
        const int h = decoded->height - y - int(decoded->crop_bottom);
        if (x < 0 || y < 0 || (x & 1) || (y & 1) || w <= 0 || h <= 0 ||
            x + w > frames->width || y + h > frames->height) {
            fail("Invalid FFmpeg Vulkan crop"); return nullptr;
        }
        AVFrame* retained = av_frame_clone(decoded);
        if (!retained) { fail("Cannot retain Vulkan decoder frame"); return nullptr; }
        auto owner = std::shared_ptr<void>(retained, [](void* frame) {
            auto* owned = static_cast<AVFrame*>(frame); av_frame_free(&owned);
        });
        vkFrames->lock_frame(frames, vk);
        auto* texture = transfer(vk->img[0], vk->layout[0], vk->sem[0], vk->sem_value[0],
            vkFrames->format[0], pixel, color, x, y, w, h, std::move(owner));
        if (texture) { ++vk->sem_value[0]; vk->access[0] = VK_ACCESS_TRANSFER_READ_BIT; }
        vkFrames->unlock_frame(frames, vk);
        return texture;
    }
#endif
};

void VulkanVideoInterop::drainRenderers() {
    for (auto* interop : interops()) interop->impl_->clear();
    for (auto& item : devices()) {
        item->clearPresentation();
        if (item->device) vkDeviceWaitIdle(item->device->device);
    }
}
void VulkanVideoInterop::releaseRenderers() {
    for (auto* interop : interops()) {
        interop->impl_->removeProbe();
        interop->impl_->context = nullptr;
        interop->impl_->renderer = nullptr;
    }
    devices().clear();
}

VulkanVideoInterop::VulkanVideoInterop(SDL_Renderer* renderer, bool ffmpeg)
    : impl_(std::make_unique<Impl>(renderer, ffmpeg)) { interops().push_back(this); }
VulkanVideoInterop::~VulkanVideoInterop() {
    discardFrames();
    impl_->removeProbe();
    if (impl_->pipeline) gst_object_unref(impl_->pipeline);
    auto& list = interops();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}
bool VulkanVideoInterop::available() const {
    return impl_->context && (impl_->ffmpeg || impl_->context->video);
}
bool VulkanVideoInterop::deferred() const { return impl_->deferred; }
const char* VulkanVideoInterop::reason() const { return impl_->error.c_str(); }
void VulkanVideoInterop::configure(GstElement* pipeline) {
    if (impl_->pipeline) gst_object_unref(impl_->pipeline);
    impl_->pipeline = pipeline ? GST_ELEMENT(gst_object_ref(pipeline)) : nullptr;
    setContext(pipeline, impl_->context);
}
void VulkanVideoInterop::configureElement(GstElement* element) {
    if (setContext(element, impl_->context))
        LOG_INFO("VulkanVideoInterop", "Applied shared Vulkan device to playbin decoder");
}
GstElement* VulkanVideoInterop::wrapSink(GstElement* sink) {
    setContext(sink, impl_->context);
    if (impl_->context && sink) {
        impl_->removeProbe();
        GstPad* pad = gst_element_get_static_pad(sink, "sink");
        if (pad) {
            impl_->probeId = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                answerContextQuery, impl_->context, nullptr);
            if (impl_->probeId) impl_->probePad = pad;
            else gst_object_unref(pad);
        }
    }
    return sink;
}
SDL_Texture* VulkanVideoInterop::copy(GstSample* sample) {
    return impl_->copy(sample);
}
#ifdef RETROFE_HAVE_FFMPEG
SDL_Texture* VulkanVideoInterop::copy(AVFrame* frame, SDL_Colorspace colorspace) {
    return impl_->copy(frame, colorspace);
}
#endif
void VulkanVideoInterop::discardFrames() {
    impl_->current = nullptr;
    impl_->collect();
}
void VulkanVideoInterop::invalidateFrame() { discardFrames(); }
bool VulkanVideoInterop::hasActiveFrame(SDL_Renderer* renderer) {
    for (auto* interop : interops())
        if (interop->impl_->renderer == renderer && interop->impl_->current) return true;
    return false;
}
bool VulkanVideoInterop::beginFrame(SDL_Renderer* renderer) {
    if (auto* device = shared(renderer)) device->collect();
    return true;
}
void VulkanVideoInterop::startFrame(SDL_Renderer* renderer) { beginFrame(renderer); }
bool VulkanVideoInterop::markForDraw(SDL_Texture*) { return true; }
bool VulkanVideoInterop::endFrame(SDL_Renderer*) { return true; }
void VulkanVideoInterop::lockPresent(SDL_Renderer* renderer) {
    if (auto* device = shared(renderer)) gst_vulkan_queue_submit_lock(device->graphics);
}
void VulkanVideoInterop::unlockPresent(SDL_Renderer* renderer) {
    if (auto* device = shared(renderer)) gst_vulkan_queue_submit_unlock(device->graphics);
}
