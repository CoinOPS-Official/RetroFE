#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>

// Main/render thread only. Decoder buffers are never CPU-mapped.
class EGLVideoInterop {
public:
    explicit EGLVideoInterop(SDL_Renderer*);
    ~EGLVideoInterop();
    bool available() const;
    void configure(GstElement*) {}
    GstElement* wrapSink(GstElement*);
    SDL_Texture* copy(GstSample*);
    void discardFrames();
    const char* reason() const;
    static const char* caps() { return "video/x-raw(memory:DMABuf),format=DMA_DRM"; }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_ABGR8888; }
    const char* description() const;
    int width() const;
    int height() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
