#pragma once
#include "D3D11VideoInterop.h"
#ifdef RETROFE_HAVE_GST_VULKAN
#include "VulkanVideoInterop.h"
#endif
#ifdef RETROFE_HAVE_D3D12
#include "D3D12VideoInterop.h"
#endif
#include <cstring>

// Match the memory contract to the actual renderer, including startup fallback.
class WindowsVideoInterop {
public:
    static bool initializeGlobal(SDL_Renderer*) { return true; }
    explicit WindowsVideoInterop(SDL_Renderer* renderer) {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (std::strcmp(SDL_GetRendererName(renderer), "vulkan") == 0) {
            vk_ = std::make_unique<VulkanVideoInterop>(renderer);
            return;
        }
#endif
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
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return vk_->available();
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->available();
#endif
        return d11_ && d11_->available();
    }
    const char* reason() const {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return vk_->reason();
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->reason();
#endif
        return d11_->reason();
    }
    void configure(GstElement* pipeline) {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) { vk_->configure(pipeline); return; }
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) { d12_->configure(pipeline); return; }
#endif
        d11_->configure(pipeline);
    }
    void configureElement(GstElement* element) {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) vk_->configureElement(element);
#else
        (void)element;
#endif
    }
    SDL_Texture* copy(GstSample* sample) {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return vk_->copy(sample);
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->copy(sample);
#endif
        return d11_->copy(sample);
    }
    void discardFrames() {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) { vk_->discardFrames(); return; }
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) { d12_->discardFrames(); return; }
#endif
        d11_->discardFrames();
    }
    void invalidateFrame() {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) { vk_->invalidateFrame(); return; }
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) d12_->invalidateFrame();
#endif
    }
    bool deferred() const {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return vk_->deferred();
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->deferred();
#endif
        return false;
    }
    bool submitsPresentation() const {
#ifdef RETROFE_HAVE_D3D12
        return d12_ != nullptr;
#else
        return false;
#endif
    }
    bool retainsDeferredFrame() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->retainsDeferredFrame();
#endif
        return false;
    }
    SDL_Texture* currentTexture() const {
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->currentTexture();
#endif
        return nullptr;
    }
    GstElement* wrapSink(GstElement* sink) {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return vk_->wrapSink(sink);
#endif
        return sink;
    }
    bool proposeAllocation(GstQuery* query) {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return vk_->proposeAllocation(query);
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return d12_->proposeAllocation(query);
#endif
        return d11_ ? d11_->proposeAllocation(query) : false;
    }
    const char* caps() const {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return VulkanVideoInterop::caps();
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return D3D12VideoInterop::caps();
#endif
        return D3D11VideoInterop::caps();
    }
    SDL_PixelFormat pixelFormat() const { return SDL_PIXELFORMAT_NV12; }
    const char* description() const {
#ifdef RETROFE_HAVE_GST_VULKAN
        if (vk_) return VulkanVideoInterop::description();
#endif
#ifdef RETROFE_HAVE_D3D12
        if (d12_) return D3D12VideoInterop::description();
#endif
        return D3D11VideoInterop::description();
    }
private:
#ifdef RETROFE_HAVE_GST_VULKAN
    std::unique_ptr<VulkanVideoInterop> vk_;
#endif
    std::unique_ptr<D3D11VideoInterop> d11_;
#ifdef RETROFE_HAVE_D3D12
    std::unique_ptr<D3D12VideoInterop> d12_;
#endif
};
