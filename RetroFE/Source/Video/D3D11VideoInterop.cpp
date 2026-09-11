#include "D3D11VideoInterop.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_4.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/video/video.h>

static ID3D11Multithread* rendererLock(SDL_Renderer* renderer) {
    auto* device = static_cast<ID3D11Device*>(SDL_GetPointerProperty(
        SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
    if (!device) return nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Multithread* lock = nullptr;
    device->GetImmediateContext(&context);
    if (context) { context->QueryInterface(IID_PPV_ARGS(&lock)); context->Release(); }
    if (lock) lock->SetMultithreadProtected(TRUE);
    return lock;
}
#endif

struct D3D11VideoInterop::Impl {
    SDL_Renderer* renderer;
    std::string error = "renderer does not expose a D3D11 device";
#ifdef _WIN32
    ID3D11Device* device = nullptr; // borrowed from the renderer
    GstD3D11Device* gstDevice = nullptr;
    GstContext* context = nullptr;
    struct Slot { SDL_Texture* texture = nullptr; ID3D11Texture2D* native = nullptr; };
    std::array<Slot, 3> slots{};
    unsigned next = 0, width = 0, height = 0;
    SDL_Colorspace colorspace = SDL_COLORSPACE_UNKNOWN;
    std::vector<GstSample*> retained;
    void clear() {
        D3D11RenderLock guard(renderer);
        SDL_FlushRenderer(renderer);
        for (auto& slot : slots) {
            if (slot.texture) SDL_DestroyTexture(slot.texture);
            if (slot.native) slot.native->Release();
            slot = {};
        }
        guard.finish();
        for (auto* sample : retained) gst_sample_unref(sample);
        retained.clear();
        width = height = next = 0;
    }
#endif
    static std::vector<Impl*>& instances() { static std::vector<Impl*> list; return list; }
    explicit Impl(SDL_Renderer* r) : renderer(r) { instances().push_back(this); }
    ~Impl() {
        std::erase(instances(), this);
#ifdef _WIN32
        clear();
        if (context) gst_context_unref(context);
        if (gstDevice) gst_object_unref(gstDevice);
#endif
    }
};

D3D11RenderLock::D3D11RenderLock(SDL_Renderer* renderer) : renderer_(renderer) {
#ifdef _WIN32
    lock_ = rendererLock(renderer);
    if (lock_) static_cast<ID3D11Multithread*>(lock_)->Enter();
#endif
}
void D3D11RenderLock::finish() {
#ifdef _WIN32
    if (lock_) {
        SDL_FlushRenderer(renderer_);
        auto* lock = static_cast<ID3D11Multithread*>(lock_);
        lock_ = nullptr;
        lock->Leave();
        lock->Release();
    }
#endif
}
D3D11RenderLock::~D3D11RenderLock() { finish(); }

D3D11VideoInterop::D3D11VideoInterop(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(renderer)) {
#ifdef _WIN32
    auto& p = *impl_;
    D3D11RenderLock guard(renderer);
    p.device = static_cast<ID3D11Device*>(SDL_GetPointerProperty(
        SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
    if (!p.device) return;
    auto* lock = rendererLock(renderer);
    if (!lock) { p.error = "D3D11 multithread protection unavailable"; return; }
    lock->Release();
    p.gstDevice = gst_d3d11_device_new_wrapped(p.device);
    if (!p.gstDevice) { p.error = "GStreamer could not wrap the renderer device"; return; }
    p.context = gst_d3d11_context_new(p.gstDevice);
    if (!p.context) p.error = "GStreamer could not create the shared-device context";
#endif
}
D3D11VideoInterop::~D3D11VideoInterop() = default;
bool D3D11VideoInterop::available() const {
#ifdef _WIN32
    return impl_->context != nullptr;
#else
    return false;
#endif
}
const char* D3D11VideoInterop::reason() const { return impl_->error.c_str(); }
void D3D11VideoInterop::configure(GstElement* pipeline) {
#ifdef _WIN32
    if (!available()) return;
    gst_element_set_context(pipeline, impl_->context);
    auto* bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, [](GstBus*, GstMessage* message, gpointer data) {
        const gchar* type = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_NEED_CONTEXT &&
            gst_message_parse_context_type(message, &type) &&
            g_strcmp0(type, GST_D3D11_DEVICE_HANDLE_CONTEXT_TYPE) == 0) {
            gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), static_cast<GstContext*>(data));
        }
        return GST_BUS_PASS;
    }, gst_context_ref(impl_->context), [](gpointer data) { gst_context_unref(static_cast<GstContext*>(data)); });
    gst_object_unref(bus);
#endif
}
SDL_Texture* D3D11VideoInterop::copy(GstSample* sample) {
#ifdef _WIN32
    auto& p = *impl_;
    GstVideoInfo info{};
    auto* buffer = gst_sample_get_buffer(sample);
    auto* caps = gst_sample_get_caps(sample);
    if (!available() || !buffer || !caps || !gst_video_info_from_caps(&info, caps)) return nullptr;
    p.error = "decoder supplied system-memory video; using CPU upload";
    if (gst_buffer_n_memory(buffer) != 1) return nullptr;
    auto* memory = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_d3d11_memory(memory)) return nullptr;
    auto* d3dMemory = GST_D3D11_MEMORY_CAST(memory);
    p.error = "decoded texture is incompatible with the SDL D3D11 device";
    if (gst_d3d11_memory_get_native_type(d3dMemory) != GST_D3D11_MEMORY_NATIVE_TYPE_TEXTURE_2D ||
        gst_d3d11_device_get_device_handle(d3dMemory->device) != p.device) return nullptr;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    if (!gst_d3d11_memory_get_texture_desc(d3dMemory, &sourceDesc) || sourceDesc.Format != DXGI_FORMAT_NV12) return nullptr;
    const bool full = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    SDL_Colorspace color = full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601)
        color = full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
        color = full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
    if (p.width != sourceDesc.Width || p.height != sourceDesc.Height || p.colorspace != color) {
        p.clear();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = sourceDesc.Width; desc.Height = sourceDesc.Height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_NV12; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (auto& slot : p.slots) {
            if (FAILED(p.device->CreateTexture2D(&desc, nullptr, &slot.native))) {
                p.error = "could not allocate shader-readable NV12 textures"; p.clear(); return nullptr;
            }
            auto props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_NV12);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, desc.Width);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, desc.Height);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, color);
            SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER, slot.native);
            slot.texture = SDL_CreateTextureWithProperties(p.renderer, props);
            SDL_DestroyProperties(props);
            if (!slot.texture) { p.error = SDL_GetError(); p.clear(); return nullptr; }
            SDL_SetTextureScaleMode(slot.texture, SDL_SCALEMODE_LINEAR);
        }
        p.width = desc.Width; p.height = desc.Height; p.colorspace = color;
    }
    D3D11RenderLock guard(p.renderer);
    // Submit any draws before overwriting a ring slot, including multiple views.
    SDL_FlushRenderer(p.renderer);
    auto& slot = p.slots[p.next++ % p.slots.size()];
    auto* context = gst_d3d11_device_get_device_context_handle(p.gstDevice);
    auto* source = gst_d3d11_memory_get_resource_handle(d3dMemory);
    context->CopySubresourceRegion(slot.native, 0, 0, 0, 0, source,
        gst_d3d11_memory_get_subresource_index(d3dMemory), nullptr);
    p.retained.push_back(gst_sample_ref(sample));
    return slot.texture;
#else
    return nullptr;
#endif
}
void D3D11VideoInterop::presented(SDL_Renderer* renderer) {
#ifdef _WIN32
    for (auto* p : Impl::instances()) if (p->renderer == renderer) {
        for (auto* sample : p->retained) gst_sample_unref(sample);
        p->retained.clear();
    }
#endif
}
