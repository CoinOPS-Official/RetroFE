#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>
#include <array>
#include <cstdint>

struct EGLDmaBufFrame {
    struct Plane { int fd; int offset; int pitch; uint64_t modifier; };
    int width = 0, height = 0;
    uint32_t fourcc = 0;
    SDL_Rect crop{};
    std::array<Plane, 4> planes{};
    int planeCount = 0;
    bool bt709 = true, fullRange = false;
    int chromaX = -1, chromaY = -1; // -1 unspecified, 0 cosited, 1 midpoint
    std::shared_ptr<void> owner; // owns FDs and decoder surface through GPU completion
};

// Main/render thread only. Decoder buffers are never CPU-mapped.
class EGLVideoInterop {
public:
    explicit EGLVideoInterop(SDL_Renderer*);
    ~EGLVideoInterop();
    bool available() const;
    void configure(GstElement*) {}
    GstElement* wrapSink(GstElement*);
    bool proposeAllocation(GstQuery* query);
    SDL_Texture* copy(GstSample*);
    SDL_Texture* copy(const EGLDmaBufFrame&);
    void discardFrames() noexcept;
    const char* reason() const;
    static const char* caps() { return "video/x-raw(memory:DMABuf),format=DMA_DRM"; }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_ABGR8888; }
    const char* description() const;
    int width() const;
    int height() const;
private:
    SDL_Texture* copyFrame(GstSample*, const EGLDmaBufFrame*);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
