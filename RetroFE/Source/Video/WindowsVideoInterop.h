#pragma once
#include "D3D11VideoInterop.h"
#ifdef RETROFE_HAVE_D3D12
#include "D3D12VideoInterop.h"
#endif
#include <cstring>

// Match the memory contract to the actual renderer, including startup fallback.
class WindowsVideoInterop {
public:
    static bool initializeGlobal(SDL_Renderer*) { return true; }
    explicit WindowsVideoInterop(SDL_Renderer* renderer) {
#ifdef RETROFE_HAVE_D3D12
        if (std::strcmp(SDL_GetRendererName(renderer), "direct3d12") == 0) {
            d12_ = std::make_unique<D3D12VideoInterop>(renderer);
            return;
        }
#endif
        D3D11VideoInterop::initializeGlobal(renderer);
        d11_ = std::make_unique<D3D11VideoInterop>(renderer);
    }
    bool available() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->available();
#endif
        return d11_ && d11_->available();
    }
    const char* reason() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->reason();
#endif
        return d11_->reason();
    }
    void configure(GstElement* pipeline) {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) { d12_->configure(pipeline); return; }
#endif
        d11_->configure(pipeline);
    }
    SDL_Texture* copy(GstSample* sample) {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->copy(sample);
#endif
        return d11_->copy(sample);
    }
    void discardFrames() {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) { d12_->discardFrames(); return; }
#endif
        d11_->discardFrames();
    }
    void invalidateFrame() {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) d12_->invalidateFrame();
#endif
    }
    bool deferred() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->deferred();
#endif
        return false;
    }
    GstElement* wrapSink(GstElement* sink) { return sink; }
    const char* caps() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return D3D12VideoInterop::caps();
#endif
        return D3D11VideoInterop::caps();
    }
    SDL_PixelFormat pixelFormat() const { return SDL_PIXELFORMAT_NV12; }
    const char* description() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return D3D12VideoInterop::description();
#endif
        return D3D11VideoInterop::description();
    }
private:
    std::unique_ptr<D3D11VideoInterop> d11_;
#ifdef RETROFE_HAVE_D3D12
    std::unique_ptr<D3D12VideoInterop> d12_;
#endif
};
