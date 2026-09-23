#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <memory>

#ifdef _WIN32
struct ID3D11Resource;
struct D3D11_TEXTURE2D_DESC;
#endif

class D3D11VideoInterop {
public:
    static bool initializeGlobal(SDL_Renderer* renderer);

    explicit D3D11VideoInterop(SDL_Renderer* renderer);
    ~D3D11VideoInterop();

    bool available() const;
    void configure(GstElement* pipeline);
    SDL_Texture* copy(GstSample* sample);
#ifdef _WIN32
    SDL_Texture* copyNative(ID3D11Resource* source, unsigned int sourceSubresource,
        const D3D11_TEXTURE2D_DESC& sourceDesc, SDL_Colorspace color);
#endif
    void discardFrames();
    const char* reason() const;
    GstElement* wrapSink(GstElement* sink);
    bool proposeAllocation(GstQuery*) { return false; }

    static const char* caps() {
        return "video/x-raw(memory:D3D11Memory),format=NV12;"
               "video/x-raw,format=NV12";
    }

    static SDL_PixelFormat pixelFormat() {
        return SDL_PIXELFORMAT_NV12;
    }

    static const char* description() {
        return "D3D11 GPU ring copy to SDL-owned NV12 textures";
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
