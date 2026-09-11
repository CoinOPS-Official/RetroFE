#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>

// All texture operations run on SDL's thread. GStreamer uses a shared context.
class GLVideoInterop {
public:
    explicit GLVideoInterop(SDL_Renderer* renderer);
    ~GLVideoInterop();
    bool available() const;
    void configure(GstElement* pipeline);
    GstElement* wrapSink(GstElement* sink);
    void discardFrames();
    SDL_Texture* copy(GstSample* sample);
    const char* reason() const;
    static const char* caps() { return "video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D,pixel-aspect-ratio=1/1"; }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_ABGR8888; }
    const char* description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
