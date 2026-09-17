#include "D3D11VideoInterop.h"
#include "../Utility/Log.h"
#include "../Utility/ThreadPool.h"
#include <string>
#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <cstdint>
#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_4.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/video/video.h>

static ID3D11Device* g_device = nullptr;
static GstD3D11Device* g_gstDevice = nullptr;
static GstContext* g_context = nullptr;
static std::mutex g_contextMutex;

namespace {
    double elapsedMs(Uint64 startNs, Uint64 endNs) {
        return static_cast<double>(endNs - startNs) / 1000000.0;
    }

    struct WrapperKey {
        uintptr_t resource = 0;
        UINT subresource = 0;
        SDL_Colorspace colorspace = SDL_COLORSPACE_UNKNOWN;

        bool operator==(const WrapperKey& other) const noexcept {
            return resource == other.resource &&
                   subresource == other.subresource &&
                   colorspace == other.colorspace;
        }
    };

    struct WrapperKeyHash {
        size_t operator()(const WrapperKey& key) const noexcept {
            size_t h = std::hash<uintptr_t>{}(key.resource);
            h ^= std::hash<UINT>{}(key.subresource) +
                 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<Uint32>{}(static_cast<Uint32>(key.colorspace)) +
                 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    void logInteropPerf(
        const char* operation,
        double totalMs,
        double phase1Ms,
        const char* phase1Name,
        double phase2Ms,
        const char* phase2Name,
        size_t pendingCount)
    {
        constexpr double SLOW_MS = 2.0;
        constexpr size_t PENDING_WARN = 4;

        if (totalMs < SLOW_MS &&
            phase1Ms < SLOW_MS &&
            phase2Ms < SLOW_MS &&
            pendingCount <= PENDING_WARN)
        {
            return;
        }

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3)
           << "op=" << operation
           << " total_ms=" << totalMs
           << " " << phase1Name << "_ms=" << phase1Ms
           << " " << phase2Name << "_ms=" << phase2Ms
           << " pending=" << pendingCount;

        LOG_DEBUG("D3D11InteropPerf", ss.str());
    }
}

bool D3D11VideoInterop::initializeGlobal(SDL_Renderer* renderer) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_context) return true;
    
    g_device = static_cast<ID3D11Device*>(SDL_GetPointerProperty(
        SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
    if (!g_device) return false;
    
    ID3D11DeviceContext* immContext = nullptr;
    g_device->GetImmediateContext(&immContext);
    if (immContext) {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(immContext->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
        immContext->Release();
    }
    
    g_gstDevice = gst_d3d11_device_new_wrapped(g_device);
    if (!g_gstDevice) return false;
    
    g_context = gst_d3d11_context_new(g_gstDevice);
    return g_context != nullptr;
}
#else
bool D3D11VideoInterop::initializeGlobal(SDL_Renderer*) { return false; }
#endif

struct D3D11VideoInterop::Impl {
    SDL_Renderer* renderer;
    std::string error = "renderer does not expose a D3D11 device";

#ifdef _WIN32
    struct PendingFrame {
        SDL_Texture* texture = nullptr;
        GstSample* sample = nullptr;
        ID3D11Query* fence = nullptr;
    };

    SDL_Texture* currentTexture = nullptr;
    GstSample* currentSample = nullptr;
    ID3D11Query* currentFence = nullptr;
    std::vector<PendingFrame> pending;

    // Decoder surfaces repeat heavily (observed 15 stable resources per
    // decoder), so keep the SDL wrapper/SRVs alive and reuse them. The key
    // includes colorspace because that is baked into SDL's YUV conversion
    // state at texture creation time.
    static constexpr size_t MAX_WRAPPER_CACHE = 32;
    std::unordered_map<WrapperKey, SDL_Texture*, WrapperKeyHash> wrapperCache;

    bool wrapperInFlight(SDL_Texture* texture) const {
        if (!texture)
            return false;

        if (currentTexture == texture)
            return true;

        for (const auto& frame : pending) {
            if (frame.texture == texture)
                return true;
        }

        return false;
    }

    void pruneWrapperCache(size_t targetSize) {
        if (wrapperCache.size() <= targetSize)
            return;

        for (auto it = wrapperCache.begin();
             it != wrapperCache.end() && wrapperCache.size() > targetSize;)
        {
            SDL_Texture* texture = it->second;
            if (wrapperInFlight(texture)) {
                ++it;
                continue;
            }

            SDL_DestroyTexture(texture);
            it = wrapperCache.erase(it);
        }
    }

    void destroyWrapperCache() {
        for (auto& entry : wrapperCache) {
            if (entry.second)
                SDL_DestroyTexture(entry.second);
        }
        wrapperCache.clear();
    }

    explicit Impl(SDL_Renderer* r) : renderer(r) {}

    ID3D11DeviceContext* context() const {
        return g_gstDevice ? gst_d3d11_device_get_device_context_handle(g_gstDevice) : nullptr;
    }

    void destroyFrame(PendingFrame& frame) {
        // frame.texture is owned by wrapperCache and is intentionally retained
        // across decoder-surface reuse. Only the per-use sample/fence retire.
        frame.texture = nullptr;

        if (frame.sample) {
            gst_sample_unref(frame.sample);
            frame.sample = nullptr;
        }
        if (frame.fence) {
            frame.fence->Release();
            frame.fence = nullptr;
        }
    }

    // SDL_Renderer batches texture reads. Before allowing GStreamer to recycle
    // the decoder surface, submit all queued reads and place a D3D11 event
    // immediately after them. The GstSample remains referenced until that event
    // completes.
    void submitCurrent() {
        if (!currentSample)
            return;

        SDL_FlushRenderer(renderer);

        auto* ctx = context();
        ctx->End(currentFence);
        // The texture pointer is non-owning; wrapperCache owns the SDL texture.
        pending.push_back({ currentTexture, currentSample, currentFence });
        currentFence = nullptr;

        currentTexture = nullptr;
        currentSample = nullptr;
    }

    void retireCompleted() {
        auto* ctx = context();
        if (!ctx)
            return;

        for (auto it = pending.begin(); it != pending.end();) {
            const HRESULT hr = ctx->GetData(
                it->fence,
                nullptr,
                0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH);

            if (hr != S_OK) {
                ++it;
                continue;
            }

            // GPU has finished sampling this decoder-owned surface. The SDL
            // wrapper stays cached; only this use's fence and GstSample retire.
            it->texture = nullptr;

            if (it->fence) {
                it->fence->Release();
                it->fence = nullptr;
            }

            // gst_sample_unref() can occasionally block for 10-20+ ms while
            // GStreamer/D3D11 recycles the decoder surface. The GPU is already
            // finished with it at this point, so move only that final release
            // off the render/main thread.
            GstSample* sampleToRelease = it->sample;
            it->sample = nullptr;

            it = pending.erase(it);

            if (sampleToRelease) {
                ThreadPool::getInstance().enqueue(
                    [sampleToRelease]() {
                        gst_sample_unref(sampleToRelease);
                    });
            }
        }

        // Normally the active decoder pool is much smaller than this limit.
        // If instant-URI retargeting replaces the pool, stale wrappers become
        // evictable once their last GPU use has retired.
        if (wrapperCache.size() > MAX_WRAPPER_CACHE)
            pruneWrapperCache(MAX_WRAPPER_CACHE);
    }

    // Normal retarget/reuse path.
    //
    // Do not wait for the GPU here. submitCurrent() places an event query after
    // SDL's queued reads and moves the texture/sample into pending. The sample
    // therefore continues to hold the decoder-owned surface alive until
    // retireCompleted() observes that the GPU has finished with it.
    //
    // This is intentionally non-blocking so rapid warm-instance retargets do
    // not stall the main thread waiting for old decoder surfaces.
    void discardFrames() {
        if (!SDL_IsMainThread()) {
            SDL_RunOnMainThread([](void* data) {
                static_cast<Impl*>(data)->discardFrames();
            }, this, true);
            return;
        }

        const Uint64 totalStartNs = SDL_GetTicksNS();

        const Uint64 submitStartNs = SDL_GetTicksNS();
        submitCurrent();
        const Uint64 submitEndNs = SDL_GetTicksNS();

        const Uint64 retireStartNs = SDL_GetTicksNS();
        retireCompleted();
        const Uint64 retireEndNs = SDL_GetTicksNS();

        const Uint64 totalEndNs = SDL_GetTicksNS();

        logInteropPerf(
            "discard",
            elapsedMs(totalStartNs, totalEndNs),
            elapsedMs(submitStartNs, submitEndNs),
            "submit",
            elapsedMs(retireStartNs, retireEndNs),
            "retire",
            pending.size());
    }

    // Final teardown path.
    //
    // Destruction is allowed to wait because the Impl and all of its retained
    // SDL textures / GstSamples must be gone before this function returns.
    void clearBlocking() {
        if (!SDL_IsMainThread()) {
            SDL_RunOnMainThread([](void* data) {
                static_cast<Impl*>(data)->clearBlocking();
            }, this, true);
            return;
        }

        submitCurrent();

        auto* ctx = context();
        if (ctx && !pending.empty()) {
            ctx->Flush();

            for (auto& frame : pending) {
                while (frame.fence &&
                       ctx->GetData(frame.fence, nullptr, 0, 0) == S_FALSE) {
                    SDL_Delay(0);
                }

                destroyFrame(frame);
            }
        } else {
            for (auto& frame : pending)
                destroyFrame(frame);
        }

        pending.clear();

        // All renderer reads are complete here, so cached SDL wrappers can be
        // safely destroyed and release their COM references to decoder textures.
        destroyWrapperCache();
        currentTexture = nullptr;
    }

    ~Impl() {
        clearBlocking();
    }
#else
    explicit Impl(SDL_Renderer* r) : renderer(r) {}
    ~Impl() = default;
#endif
};

D3D11VideoInterop::D3D11VideoInterop(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(renderer)) {
#ifdef _WIN32
    if (!g_context) { impl_->error = "Global D3D11 context not initialized"; return; }
#endif
}
D3D11VideoInterop::~D3D11VideoInterop() = default;

void D3D11VideoInterop::discardFrames() {
#ifdef _WIN32
    impl_->discardFrames();
#endif
}

bool D3D11VideoInterop::available() const {
#ifdef _WIN32
    return g_context != nullptr;
#else
    return false;
#endif
}

const char* D3D11VideoInterop::reason() const { return impl_->error.c_str(); }

void D3D11VideoInterop::configure(GstElement* pipeline) {
#ifdef _WIN32
    if (!available()) return;
    gst_element_set_context(pipeline, g_context);
    auto* bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, [](GstBus*, GstMessage* message, gpointer data) {
        const gchar* type = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_NEED_CONTEXT &&
            gst_message_parse_context_type(message, &type) &&
            g_strcmp0(type, GST_D3D11_DEVICE_HANDLE_CONTEXT_TYPE) == 0) {
            gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), static_cast<GstContext*>(data));
        }
        return GST_BUS_PASS;
        }, gst_context_ref(g_context), [](gpointer data) { gst_context_unref(static_cast<GstContext*>(data)); });
    gst_object_unref(bus);
#endif
}

GstElement* D3D11VideoInterop::wrapSink(GstElement* sink) {
    return sink; // No longer forcing d3d11colorconvert since we manually extract slices
}

SDL_Texture* D3D11VideoInterop::copy(GstSample* sample) {
#ifdef _WIN32
    auto& p = *impl_;

    GstVideoInfo info{};
    auto* buffer = gst_sample_get_buffer(sample);
    auto* caps = gst_sample_get_caps(sample);
    if (!available() || !buffer || !caps || !gst_video_info_from_caps(&info, caps))
        return nullptr;

    p.error = "decoder supplied system-memory video; using CPU upload";
    if (gst_buffer_n_memory(buffer) != 1)
        return nullptr;

    auto* memory = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_d3d11_memory(memory))
        return nullptr;

    auto* d3dMemory = GST_D3D11_MEMORY_CAST(memory);

    p.error = "decoded texture is incompatible with the SDL D3D11 device";
    if (gst_d3d11_memory_get_native_type(d3dMemory) != GST_D3D11_MEMORY_NATIVE_TYPE_TEXTURE_2D ||
        gst_d3d11_device_get_device_handle(d3dMemory->device) != g_device) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    if (!gst_d3d11_memory_get_texture_desc(d3dMemory, &sourceDesc) ||
        sourceDesc.Format != DXGI_FORMAT_NV12) {
        p.error = "decoder did not supply an NV12 D3D11 Texture2D";
        return nullptr;
    }

    const UINT subresource =
        gst_d3d11_memory_get_subresource_index(d3dMemory);

    const UINT subresourceCount =
        sourceDesc.MipLevels * sourceDesc.ArraySize;

    if (subresource >= subresourceCount) {
        p.error = "decoder supplied an invalid D3D11 subresource index";
        return nullptr;
    }

    const Uint64 retireStartNs = SDL_GetTicksNS();
    p.retireCompleted();
    const double retireMs =
        elapsedMs(retireStartNs, SDL_GetTicksNS());

    auto* resource =
        gst_d3d11_memory_get_resource_handle(d3dMemory);

    if (!resource) {
        p.error = "decoder D3D11 memory has no native resource";
        return nullptr;
    }

    ID3D11Texture2D* sourceTexture = nullptr;
    if (FAILED(resource->QueryInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&sourceTexture))) ||
        !sourceTexture) {
        p.error = "decoder D3D11 resource is not an ID3D11Texture2D";
        return nullptr;
    }

    const bool full =
        info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;

    SDL_Colorspace color =
        full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;

    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601)
        color = full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;

    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
        color = full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;

    const WrapperKey key{
        reinterpret_cast<uintptr_t>(sourceTexture),
        subresource,
        color
    };

    const Uint64 importStartNs = SDL_GetTicksNS();

    SDL_Texture* texture = nullptr;
    bool insertedWrapper = false;
    auto cached = p.wrapperCache.find(key);

    if (cached != p.wrapperCache.end()) {
        texture = cached->second;
    } else {
        // Make room before adding a new decoder pool entry. Only wrappers with
        // no current/pending GPU use are eligible for eviction.
        if (p.wrapperCache.size() >= Impl::MAX_WRAPPER_CACHE)
            p.pruneWrapperCache(Impl::MAX_WRAPPER_CACHE - 1);

        SDL_PropertiesID props = SDL_CreateProperties();
        if (!props) {
            sourceTexture->Release();
            p.error = SDL_GetError();
            return nullptr;
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
            sourceDesc.Width);

        SDL_SetNumberProperty(
            props,
            SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
            sourceDesc.Height);

        SDL_SetNumberProperty(
            props,
            SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
            color);

        SDL_SetPointerProperty(
            props,
            SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,
            sourceTexture);

        SDL_SetNumberProperty(
            props,
            SDL_PROP_TEXTURE_CREATE_D3D11_SUBRESOURCE_NUMBER,
            static_cast<Sint64>(subresource));

        texture = SDL_CreateTextureWithProperties(p.renderer, props);
        SDL_DestroyProperties(props);

        if (!texture) {
            sourceTexture->Release();
            p.error = SDL_GetError();
            return nullptr;
        }

        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

        p.wrapperCache.emplace(key, texture);
        insertedWrapper = true;
    }

    const double importMs =
        elapsedMs(importStartNs, SDL_GetTicksNS());

    // SDL's cached wrapper owns its own COM reference to the decoder texture.
    sourceTexture->Release();

    // Allocate the completion query before exposing the decoder surface to SDL.
    // A context Flush alone cannot make early sample release safe.
    ID3D11Query* fence = nullptr;
    D3D11_QUERY_DESC queryDesc{};
    queryDesc.Query = D3D11_QUERY_EVENT;
    if (!p.context() || FAILED(g_device->CreateQuery(&queryDesc, &fence))) {
        if (insertedWrapper) {
            auto it = p.wrapperCache.find(key);
            if (it != p.wrapperCache.end() && it->second == texture)
                p.wrapperCache.erase(it);
            SDL_DestroyTexture(texture);
        }
        p.error = "could not create decoder surface completion query";
        return nullptr;
    }

    // Keep the last texture valid if importing its replacement fails.
    const Uint64 submitStartNs = SDL_GetTicksNS();
    p.submitCurrent();
    const double submitMs =
        elapsedMs(submitStartNs, SDL_GetTicksNS());

    p.currentFence = fence;

    // "sync_total_ms" is the sum of the synchronization-sensitive phases,
    // rather than the entire copy() function.
    const double diagnosticTotalMs =
        retireMs + importMs + submitMs;

    constexpr double SLOW_MS = 2.0;
    constexpr size_t PENDING_WARN = 4;

    if (diagnosticTotalMs >= SLOW_MS ||
        retireMs >= SLOW_MS ||
        importMs >= SLOW_MS ||
        submitMs >= SLOW_MS ||
        p.pending.size() > PENDING_WARN)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3)
           << "op=copy"
           << " sync_total_ms=" << diagnosticTotalMs
           << " retire_ms=" << retireMs
           << " import_ms=" << importMs
           << " submit_ms=" << submitMs
           << " pending=" << p.pending.size();

        LOG_DEBUG("D3D11InteropPerf", ss.str());
    }

    // GStreamerVideo drops its reference immediately after copy() succeeds.
    // Hold our own reference until the renderer has finished sampling this
    // decoder-owned subresource.
    p.currentSample = gst_sample_ref(sample);
    p.currentTexture = texture;

    p.error.clear();
    return texture;
#else
    (void)sample;
    return nullptr;
#endif
}
