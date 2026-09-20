#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>

// All methods except the GStreamer context callback run on the render thread.
// copy() prepares a frame; beginFrame() submits transfers before SDL draws.
class D3D12VideoInterop {
public:
    explicit D3D12VideoInterop(SDL_Renderer* renderer);
    ~D3D12VideoInterop();
    bool available() const;
    const char* reason() const;
    void configure(GstElement* pipeline);
    GstElement* wrapSink(GstElement* sink) { return sink; }
    SDL_Texture* copy(GstSample* sample);
    void discardFrames();
    void invalidateFrame();
    bool deferred() const;
    static bool beginFrame(SDL_Renderer* renderer);
    static const char* caps() {
        return "video/x-raw(memory:D3D12Memory),format=NV12;video/x-raw,format=NV12";
    }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_NV12; }
    static const char* description() {
        return "D3D12 NV12 GPU copy ring; frame-boundary submission and GPU fences";
    }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
