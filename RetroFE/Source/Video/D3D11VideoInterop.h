#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>

// Main-thread texture ownership; streaming threads receive only a GstContext.
class D3D11VideoInterop {
public:
    explicit D3D11VideoInterop(SDL_Renderer* renderer);
    ~D3D11VideoInterop();
    bool available() const;
    void configure(GstElement* pipeline);
    SDL_Texture* copy(GstSample* sample);
    const char* reason() const;
    GstElement* wrapSink(GstElement* sink) { return sink; }
    static const char* caps() { return "video/x-raw(memory:D3D11Memory),format=NV12,pixel-aspect-ratio=1/1;video/x-raw,format=NV12,pixel-aspect-ratio=1/1"; }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_NV12; }
    static const char* description() { return "D3D11 NV12 GPU copy to SDL3 texture, no CPU readback/upload"; }
    static void presented(SDL_Renderer* renderer);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Protect an entire SDL command batch, then release before vsync.
class D3D11RenderLock {
public:
    explicit D3D11RenderLock(SDL_Renderer* renderer);
    ~D3D11RenderLock();
    void finish();
private:
    SDL_Renderer* renderer_;
    void* lock_ = nullptr;
};
