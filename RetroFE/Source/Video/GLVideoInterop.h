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
    // RETROFE_GL_DIRECT=1 requests native NV12 GLMemory. The default keeps
    // the proven RGBA GPU-copy compatibility path.
    static const char* caps();
    static SDL_PixelFormat pixelFormat();
    const char* description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
