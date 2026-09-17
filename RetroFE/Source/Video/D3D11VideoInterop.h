#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>

class D3D11VideoInterop {
public:
    static bool initializeGlobal(SDL_Renderer* renderer);

    explicit D3D11VideoInterop(SDL_Renderer* renderer);
    ~D3D11VideoInterop();
    bool available() const;
    void configure(GstElement* pipeline);
    SDL_Texture* copy(GstSample* sample);
    void discardFrames();
    const char* reason() const;
    GstElement* wrapSink(GstElement* sink);
    static const char* caps() { return "video/x-raw(memory:D3D11Memory),format=NV12;video/x-raw,format=NV12"; }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_NV12; }
    static const char* description() { return "D3D11 zero-copy decoder subresource wrap"; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};