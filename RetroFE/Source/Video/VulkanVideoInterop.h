#pragma once

#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <vulkan/vulkan.h>
#include <memory>

#ifdef RETROFE_HAVE_FFMPEG
extern "C" {
struct AVBufferRef;
struct AVFrame;
}
#endif

// The Vulkan device is created before SDL's renderer so the decoder and SDL
// can use the same VkImage handles. Other decoders can use sharedDevice() to
// bind to the same Vulkan device; the GstSample adapter is below.
class VulkanVideoInterop {
public:
    struct DeviceInfo {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        VkQueue videoQueue = VK_NULL_HANDLE;
        uint32_t graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
    };
    static SDL_Renderer* createRenderer(SDL_Window* window);
    static void drainRenderers(); // call before SDL_DestroyRenderer
    static void releaseRenderers(); // call after SDL_DestroyRenderer
    static bool hasSharedDevice(SDL_Renderer* renderer);
    static bool hasActiveFrame(SDL_Renderer* renderer);
    static bool supportsVideo(SDL_Renderer* renderer);
    static bool sharedDevice(SDL_Renderer* renderer, DeviceInfo& info);
#ifdef RETROFE_HAVE_FFMPEG
    // FFmpeg owns a reference to the shared SDL Vulkan device context.
    static AVBufferRef* createFFmpegDevice(SDL_Renderer* renderer);
#endif
    static void startFrame(SDL_Renderer* renderer);
    // Compatibility hooks; presentation copies are submitted before SDL draws.
    static bool markForDraw(SDL_Texture* texture);
    static bool beginFrame(SDL_Renderer* renderer);
    static bool endFrame(SDL_Renderer* renderer);
    static void lockPresent(SDL_Renderer* renderer);
    static void unlockPresent(SDL_Renderer* renderer);

    explicit VulkanVideoInterop(SDL_Renderer* renderer, bool ffmpeg = false);
    ~VulkanVideoInterop();
    bool available() const;
    const char* reason() const;
    void configure(GstElement* pipeline);
    void configureElement(GstElement* element);
    GstElement* wrapSink(GstElement* sink);
    bool proposeAllocation(GstQuery*) { return false; }
    // Call during frame updates before recording SDL draws. Deferred frames
    // return null without replacing the previous presentation.
    SDL_Texture* copy(GstSample* sample);
#ifdef RETROFE_HAVE_FFMPEG
    SDL_Texture* copy(AVFrame* frame, SDL_Colorspace colorspace);
#endif
    void discardFrames();
    void invalidateFrame();
    bool deferred() const;
    static const char* caps() {
        return "video/x-raw(memory:VulkanImage),format=NV12";
    }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_NV12; }
    static const char* description() {
        return "Vulkan NV12 GPU copy to persistent presentation textures";
    }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
