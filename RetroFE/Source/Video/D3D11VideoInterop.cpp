#include "D3D11VideoInterop.h"
#include "../Utility/Log.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/video/video.h>

namespace {
    ID3D11Device* g_device = nullptr;          // borrowed from SDL renderer
    GstD3D11Device* g_gstDevice = nullptr;
    GstContext* g_context = nullptr;
    std::mutex g_contextMutex;

    constexpr size_t RING_SIZE = 3;
    constexpr double SLOW_MS = 2.0;

    double elapsedMs(Uint64 startNs, Uint64 endNs)
    {
        return static_cast<double>(endNs - startNs) / 1000000.0;
    }

    SDL_Colorspace chooseColorspace(const GstVideoInfo& info)
    {
        const bool full =
            info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;

        SDL_Colorspace color =
            full ? SDL_COLORSPACE_BT709_FULL
                 : SDL_COLORSPACE_BT709_LIMITED;

        if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601) {
            color =
                full ? SDL_COLORSPACE_BT601_FULL
                     : SDL_COLORSPACE_BT601_LIMITED;
        } else if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020) {
            color =
                full ? SDL_COLORSPACE_BT2020_FULL
                     : SDL_COLORSPACE_BT2020_LIMITED;
        }

        return color;
    }
}
#endif

bool D3D11VideoInterop::initializeGlobal(SDL_Renderer* renderer)
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_contextMutex);

    if (g_context)
        return true;

    g_device = static_cast<ID3D11Device*>(
        SDL_GetPointerProperty(
            SDL_GetRendererProperties(renderer),
            SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,
            nullptr));

    if (!g_device) {
        LOG_ERROR(
            "D3D11VideoInterop",
            "SDL renderer does not expose a D3D11 device.");
        return false;
    }

    // SDL and GStreamer use the same D3D11 device/immediate context. Keep
    // D3D11's built-in multithread protection enabled because GStreamer can
    // issue D3D11 work from streaming threads while SDL renders on the main
    // thread.
    ID3D11DeviceContext* immediate = nullptr;
    g_device->GetImmediateContext(&immediate);

    if (immediate) {
        ID3D10Multithread* mt = nullptr;

        if (SUCCEEDED(
                immediate->QueryInterface(
                    __uuidof(ID3D10Multithread),
                    reinterpret_cast<void**>(&mt))) &&
            mt)
        {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }

        immediate->Release();
    }

    g_gstDevice = gst_d3d11_device_new_wrapped(g_device);
    if (g_gstDevice) {
        g_context = gst_d3d11_context_new(g_gstDevice);
        if (!g_context) {
            gst_object_unref(g_gstDevice);
            g_gstDevice = nullptr;
            LOG_WARNING(
                "D3D11VideoInterop",
                "Could not create GStreamer D3D11 context; GStreamer D3D11 interop disabled.");
        }
    } else {
        LOG_WARNING(
            "D3D11VideoInterop",
            "Could not wrap SDL D3D11 device for GStreamer; GStreamer D3D11 interop disabled.");
    }

    LOG_INFO(
        "D3D11VideoInterop",
        "Using D3D11 owned-NV12 ring copy path "
        "(3 SDL-owned textures; no decoder-surface retention/query/ID3D11DeviceContext::Flush).");

    return true;
#else
    (void)renderer;
    return false;
#endif
}

struct D3D11VideoInterop::Impl
{
    explicit Impl(SDL_Renderer* r)
        : renderer(r)
    {
#ifdef _WIN32
        rendererDevice = static_cast<ID3D11Device*>(
            SDL_GetPointerProperty(
                SDL_GetRendererProperties(renderer),
                SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,
                nullptr));
        if (rendererDevice) {
            rendererDevice->GetImmediateContext(rendererContext.GetAddressOf());
        }
#endif
    }

    ~Impl()
    {
#ifdef _WIN32
        clearSlots();
#endif
    }

    SDL_Renderer* renderer = nullptr;
    std::string error = "D3D11 ring-copy interop unavailable";

#ifdef _WIN32
    struct Slot
    {
        SDL_Texture* texture = nullptr;

        // Borrowed from SDL_Texture. SDL owns the COM reference and keeps this
        // valid for the lifetime of texture.
        ID3D11Texture2D* nativeTexture = nullptr;
    };

    ID3D11Device* rendererDevice = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> rendererContext;
    std::array<Slot, RING_SIZE> slots{};
    size_t nextSlot = 0;

    UINT width = 0;
    UINT height = 0;
    SDL_Colorspace colorspace = SDL_COLORSPACE_UNKNOWN;

    uint64_t copyCount = 0;

    ID3D11DeviceContext* context() const
    {
        if (rendererContext)
            return rendererContext.Get();
        return g_gstDevice
            ? gst_d3d11_device_get_device_context_handle(g_gstDevice)
            : nullptr;
    }

    void destroySlot(Slot& slot)
    {
        // nativeTexture is borrowed from the SDL texture; do not Release().
        slot.nativeTexture = nullptr;

        if (slot.texture) {
            SDL_DestroyTexture(slot.texture);
            slot.texture = nullptr;
        }
    }

    void clearSlots()
    {
        if (!SDL_IsMainThread()) {
            SDL_RunOnMainThread(
                [](void* data) {
                    static_cast<Impl*>(data)->clearSlots();
                },
                this,
                true);
            return;
        }

        // If SDL still has queued draws that reference one of our ring
        // textures, emit those D3D11 commands before releasing the SDL
        // wrappers/resources. No GPU completion wait is required here.
        if (renderer)
            SDL_FlushRenderer(renderer);

        for (auto& slot : slots)
            destroySlot(slot);

        width = 0;
        height = 0;
        colorspace = SDL_COLORSPACE_UNKNOWN;
        nextSlot = 0;
    }

    bool allocateSlots(
        UINT newWidth,
        UINT newHeight,
        SDL_Colorspace newColorspace)
    {
        if (!SDL_IsMainThread()) {
            error = "D3D11 ring textures must be allocated on the SDL main thread";
            return false;
        }

        if (!rendererDevice || rendererDevice != g_device) {
            error =
                "SDL renderer D3D11 device does not match the GStreamer D3D11 device";
            return false;
        }

        clearSlots();

        const Uint64 allocStartNs = SDL_GetTicksNS();

        for (auto& slot : slots) {
            SDL_PropertiesID props = SDL_CreateProperties();
            if (!props) {
                error = SDL_GetError();
                clearSlots();
                return false;
            }

            SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                SDL_PIXELFORMAT_NV12);

            SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                SDL_TEXTUREACCESS_STATIC);

            SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                newWidth);

            SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                newHeight);

            SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                newColorspace);

            slot.texture =
                SDL_CreateTextureWithProperties(
                    renderer,
                    props);

            SDL_DestroyProperties(props);

            if (!slot.texture) {
                error = SDL_GetError();
                clearSlots();
                return false;
            }

            SDL_SetTextureScaleMode(
                slot.texture,
                SDL_SCALEMODE_LINEAR);

            SDL_PropertiesID textureProps =
                SDL_GetTextureProperties(slot.texture);

            slot.nativeTexture =
                static_cast<ID3D11Texture2D*>(
                    SDL_GetPointerProperty(
                        textureProps,
                        SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER,
                        nullptr));

            if (!slot.nativeTexture) {
                error =
                    "SDL-created NV12 texture does not expose an ID3D11Texture2D";
                clearSlots();
                return false;
            }

            D3D11_TEXTURE2D_DESC desc{};
            slot.nativeTexture->GetDesc(&desc);

            if (desc.Format != DXGI_FORMAT_NV12 ||
                desc.ArraySize != 1 ||
                desc.MipLevels != 1 ||
                desc.Width != newWidth ||
                desc.Height != newHeight)
            {
                std::ostringstream ss;
                ss << "SDL-created ring texture has unexpected D3D11 layout"
                   << " format=" << static_cast<int>(desc.Format)
                   << " array=" << desc.ArraySize
                   << " mips=" << desc.MipLevels
                   << " size=" << desc.Width << "x" << desc.Height
                   << " expected=" << newWidth << "x" << newHeight;

                error = ss.str();
                clearSlots();
                return false;
            }
        }

        width = newWidth;
        height = newHeight;
        colorspace = newColorspace;
        nextSlot = 0;

        const double allocMs =
            elapsedMs(allocStartNs, SDL_GetTicksNS());

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3)
           << "Allocated " << RING_SIZE
           << "-slot SDL-owned NV12 ring "
           << width << "x" << height
           << " alloc_ms=" << allocMs;

        LOG_DEBUG("D3D11VideoInterop", ss.str());

        return true;
    }

    SDL_Texture* copyFromDecoder(
        ID3D11Resource* source,
        UINT sourceSubresource,
        const D3D11_TEXTURE2D_DESC& sourceDesc,
        SDL_Colorspace color)
    {
        if (!SDL_IsMainThread()) {
            error = "D3D11 ring copy must run on the SDL main thread";
            return nullptr;
        }

        if (width != sourceDesc.Width ||
            height != sourceDesc.Height ||
            colorspace != color)
        {
            if (!allocateSlots(
                    sourceDesc.Width,
                    sourceDesc.Height,
                    color))
            {
                return nullptr;
            }
        }

        ID3D11DeviceContext* ctx = context();
        if (!ctx) {
            error = "GStreamer D3D11 immediate context unavailable";
            return nullptr;
        }

        Slot& slot = slots[nextSlot];
        const size_t slotIndex = nextSlot;
        nextSlot = (nextSlot + 1) % slots.size();

        if (!slot.texture || !slot.nativeTexture) {
            error = "D3D11 ring slot is not initialized";
            return nullptr;
        }

        const Uint64 totalStartNs = SDL_GetTicksNS();

        // The slot may still be referenced by SDL draw commands from several
        // UI frames ago. Make SDL emit those commands before we enqueue the
        // overwrite. Because both operations use the same D3D11 immediate
        // context, command ordering guarantees the old reads precede this copy.
        // In multi-instance playback, coalesce flushes occurring within 2 ms
        // on the same renderer to avoid repeated UI stalls.
        const Uint64 sdlFlushStartNs = SDL_GetTicksNS();
        static thread_local Uint64 s_lastFlushNs = 0;
        static thread_local SDL_Renderer* s_lastFlushedRenderer = nullptr;

        if (s_lastFlushedRenderer != renderer || (sdlFlushStartNs - s_lastFlushNs) > 2000000ULL) {
            if (!SDL_FlushRenderer(renderer)) {
                error = SDL_GetError();
                return nullptr;
            }
            s_lastFlushNs = SDL_GetTicksNS();
            s_lastFlushedRenderer = renderer;
        }
        const Uint64 sdlFlushEndNs = SDL_GetTicksNS();


        const Uint64 copyStartNs = SDL_GetTicksNS();

        ctx->CopySubresourceRegion(
            slot.nativeTexture,
            0,
            0,
            0,
            0,
            source,
            sourceSubresource,
            nullptr);

        const Uint64 copyEndNs = SDL_GetTicksNS();

        const Uint64 totalEndNs = SDL_GetTicksNS();

        const double sdlFlushMs =
            elapsedMs(sdlFlushStartNs, sdlFlushEndNs);
        const double copyCallMs =
            elapsedMs(copyStartNs, copyEndNs);
        const double totalMs =
            elapsedMs(totalStartNs, totalEndNs);

        ++copyCount;

        if (totalMs >= SLOW_MS ||
            sdlFlushMs >= SLOW_MS ||
            copyCallMs >= SLOW_MS)
        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(3)
               << "op=ringCopy"
               << " total_ms=" << totalMs
               << " sdl_flush_ms=" << sdlFlushMs
               << " copy_call_ms=" << copyCallMs
               << " slot=" << slotIndex
               << " copies=" << copyCount;

            LOG_DEBUG("D3D11InteropPerf", ss.str());
        }

        return slot.texture;
    }
#endif
};

D3D11VideoInterop::D3D11VideoInterop(SDL_Renderer* renderer)
    : impl_(std::make_unique<Impl>(renderer))
{
#ifdef _WIN32
    if (!g_device) {
        impl_->error = "Global D3D11 device not initialized";
    } else if (!impl_->rendererDevice) {
        impl_->error = "Renderer does not expose a D3D11 device";
    } else if (impl_->rendererDevice != g_device) {
        impl_->error =
            "Renderer D3D11 device does not match global D3D11 device";
    }
#endif
}

D3D11VideoInterop::~D3D11VideoInterop() = default;

bool D3D11VideoInterop::available() const
{
#ifdef _WIN32
    return
        impl_ &&
        g_device != nullptr &&
        impl_->rendererDevice == g_device;
#else
    return false;
#endif
}

#ifdef _WIN32
SDL_Texture* D3D11VideoInterop::copyNative(
    ID3D11Resource* source,
    UINT sourceSubresource,
    const D3D11_TEXTURE2D_DESC& sourceDesc,
    SDL_Colorspace color)
{
    if (!source || !available() || !impl_)
        return nullptr;

    return impl_->copyFromDecoder(source, sourceSubresource, sourceDesc, color);
}
#endif

const char* D3D11VideoInterop::reason() const
{
    return impl_->error.c_str();
}

void D3D11VideoInterop::configure(GstElement* pipeline)
{
#ifdef _WIN32
    if (!available() || !pipeline)
        return;

    gst_element_set_context(
        pipeline,
        g_context);

    GstBus* bus = gst_element_get_bus(pipeline);
    if (!bus)
        return;

    gst_bus_set_sync_handler(
        bus,
        [](GstBus*, GstMessage* message, gpointer data) {
            const gchar* type = nullptr;

            if (GST_MESSAGE_TYPE(message) ==
                    GST_MESSAGE_NEED_CONTEXT &&
                gst_message_parse_context_type(
                    message,
                    &type) &&
                g_strcmp0(
                    type,
                    GST_D3D11_DEVICE_HANDLE_CONTEXT_TYPE) == 0)
            {
                gst_element_set_context(
                    GST_ELEMENT(GST_MESSAGE_SRC(message)),
                    static_cast<GstContext*>(data));
            }

            return GST_BUS_PASS;
        },
        gst_context_ref(g_context),
        [](gpointer data) {
            gst_context_unref(
                static_cast<GstContext*>(data));
        });

    gst_object_unref(bus);
#else
    (void)pipeline;
#endif
}

GstElement* D3D11VideoInterop::wrapSink(GstElement* sink)
{
    // Preserve the current D3D11Memory/NV12 negotiation. The pool break happens
    // here in the application by copying into SDL-owned standalone textures.
    return sink;
}

void D3D11VideoInterop::discardFrames()
{
    // There are no decoder-owned samples, event queries, or pending GPU uses
    // retained by this implementation. GStreamerVideo owns the incoming sample
    // only until copy() returns, so there is nothing to retire here.
}

SDL_Texture* D3D11VideoInterop::copy(GstSample* sample)
{
#ifdef _WIN32
    if (!sample || !available() || !impl_)
        return nullptr;

    if (!g_gstDevice || !g_context) {
        impl_->error = "GStreamer D3D11 device/context not initialized";
        return nullptr;
    }

    auto& p = *impl_;

    GstBuffer* buffer =
        gst_sample_get_buffer(sample);

    GstCaps* caps =
        gst_sample_get_caps(sample);

    GstVideoInfo info{};

    if (!buffer ||
        !caps ||
        !gst_video_info_from_caps(&info, caps))
    {
        p.error = "Could not parse D3D11 video sample";
        return nullptr;
    }

    p.error = "Decoder supplied system-memory video; using CPU upload";

    if (gst_buffer_n_memory(buffer) != 1)
        return nullptr;

    GstMemory* memory =
        gst_buffer_peek_memory(buffer, 0);

    if (!gst_is_d3d11_memory(memory))
        return nullptr;

    auto* d3dMemory =
        GST_D3D11_MEMORY_CAST(memory);

    p.error =
        "Decoded texture is incompatible with the SDL D3D11 device";

    if (gst_d3d11_memory_get_native_type(d3dMemory) !=
            GST_D3D11_MEMORY_NATIVE_TYPE_TEXTURE_2D ||
        gst_d3d11_device_get_device_handle(
            d3dMemory->device) != g_device)
    {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};

    if (!gst_d3d11_memory_get_texture_desc(
            d3dMemory,
            &sourceDesc) ||
        sourceDesc.Format != DXGI_FORMAT_NV12)
    {
        p.error =
            "Decoder did not supply an NV12 D3D11 Texture2D";
        return nullptr;
    }

    const UINT sourceSubresource =
        gst_d3d11_memory_get_subresource_index(
            d3dMemory);

    const UINT sourceSubresourceCount =
        sourceDesc.MipLevels * sourceDesc.ArraySize;

    if (sourceSubresource >= sourceSubresourceCount) {
        p.error =
            "Decoder supplied an invalid D3D11 subresource index";
        return nullptr;
    }

    ID3D11Resource* source =
        gst_d3d11_memory_get_resource_handle(
            d3dMemory);

    if (!source) {
        p.error =
            "Decoder D3D11 memory has no native resource";
        return nullptr;
    }

    const SDL_Colorspace color =
        chooseColorspace(info);

    SDL_Texture* texture =
        p.copyFromDecoder(
            source,
            sourceSubresource,
            sourceDesc,
            color);

    if (!texture)
        return nullptr;

    // Crucially, this class does NOT gst_sample_ref(sample). GStreamerVideo
    // drops its existing reference immediately after copy() succeeds.
    //
    // The CopySubresourceRegion command has already been enqueued while the
    // sample was alive and while holding the GstD3D11Device lock. Subsequent
    // decoder reuse is therefore ordered after the copy on the shared D3D11
    // device/context, matching GStreamer's own D3D11 decoder copy strategy.
    p.error.clear();
    return texture;
#else
    (void)sample;
    return nullptr;
#endif
}
