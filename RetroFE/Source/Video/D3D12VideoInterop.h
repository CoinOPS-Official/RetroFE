#pragma once
#include <SDL3/SDL.h>
#include <gst/gst.h>
#include <cstdint>
#include <memory>
struct ID3D12Resource;
struct ID3D12Fence;

// All methods except the GStreamer context callback run on the render thread.
// copy()/copyNative() prepare a frame; beginFrame() submits transfers before SDL draws.
// FFmpeg-native frames that are not producer-ready are deferred without stalling
// SDL's D3D12 graphics queue; GStreamer retains its existing queue-wait contract.
class D3D12VideoInterop {
public:
    explicit D3D12VideoInterop(SDL_Renderer* renderer);
    ~D3D12VideoInterop();
    bool available() const;
    const char* reason() const;
    void configure(GstElement* pipeline);
    GstElement* wrapSink(GstElement* sink) { return sink; }
    SDL_Texture* copy(GstSample* sample);
    // Decoder-neutral entry point. Resource must belong to SDL's device, be
    // NV12, be in COMMON once producer/value is reached, and remain alive
    // through owner until the copy fence retires it.
    SDL_Texture* copyNative(ID3D12Resource* resource, ID3D12Fence* producer,
        uint64_t value, unsigned yPlane, unsigned uvPlane, int width, int height,
        SDL_Colorspace color, std::shared_ptr<void> owner);
    void discardFrames();
    void invalidateFrame();
    bool deferred() const;
    SDL_Texture* currentTexture() const; // only a successfully submitted presentation
    static bool beginFrame(SDL_Renderer* renderer);
    static const char* caps() {
        return "video/x-raw(memory:D3D12Memory),format=NV12;video/x-raw,format=NV12";
    }
    static SDL_PixelFormat pixelFormat() { return SDL_PIXELFORMAT_NV12; }
    static const char* description() {
        return "D3D12 NV12 GPU copy ring; nonblocking FFmpeg fence polling";
    }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
