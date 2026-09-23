#include "D3D12VideoInterop.h"
#include "../Utility/Log.h"
#include <gst/d3d12/gstd3d12.h>
#include <gst/video/video.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        throw std::runtime_error(message.str());
    }
}
void checkSDL(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
SDL_Colorspace colorspace(const GstVideoInfo& info) {
    const bool full = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601)
        return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
        return full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
    return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* texture,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
struct SampleDelete { void operator()(GstSample* s) const { if (s) gst_sample_unref(s); } };
using Sample = std::unique_ptr<GstSample, SampleDelete>;
}

struct D3D12VideoInterop::Impl {
    // Registry and slots are render-thread-only. Context callbacks own their
    // own GstContext reference and never access this object.
    static std::mutex& instancesMutex() { static std::mutex mtx; return mtx; }
    static std::vector<Impl*>& instances() { static std::vector<Impl*> list; return list; }
    struct Slot {
        SDL_Texture* texture = nullptr;
        ID3D12Resource* destination = nullptr; // borrowed from SDL texture
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> source;
        ComPtr<ID3D12Fence> producerFence;
        UINT64 producerValue = 0;
        UINT64 flight = 0;
        Sample sample;
        std::shared_ptr<void> nativeOwner;
        std::array<UINT, 2> planes{};
        int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
        bool pending = false;
        bool native = false; // FFmpeg-native frame; never stall SDL's queue waiting for it.
    };
    SDL_Renderer* renderer;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    GstD3D12Device* gstDevice = nullptr;
    GstContext* context = nullptr;
    HANDLE event = nullptr;
    UINT64 sequence = 0;
    std::array<Slot, 3> slots;
    struct CachedFence {
        ComPtr<ID3D12Fence> source;
        ComPtr<ID3D12Fence> imported;
    };
    std::unordered_map<ID3D12Fence*, CachedFence> fenceCache;
    int width = 0, height = 0;
    SDL_Colorspace color = SDL_COLORSPACE_UNKNOWN;
    SDL_Texture* current = nullptr;
    std::string error = "D3D12 interop unavailable";
    bool ready = false;
    bool failedSubmission = false;
    bool deferred = false;
    const gint64 resourceToken = gst_d3d12_create_user_token();

    explicit Impl(SDL_Renderer* r) : renderer(r) {
        {
            std::lock_guard<std::mutex> lock(instancesMutex());
            instances().push_back(this);
        }
        try {
            const auto props = SDL_GetRendererProperties(r);
            device = static_cast<ID3D12Device*>(SDL_GetPointerProperty(props,
                SDL_PROP_RENDERER_D3D12_DEVICE_POINTER, nullptr));
            queue = static_cast<ID3D12CommandQueue*>(SDL_GetPointerProperty(props,
                SDL_PROP_RENDERER_D3D12_COMMAND_QUEUE_POINTER, nullptr));
            if (!device || !queue) throw std::runtime_error("Renderer has no D3D12 device/queue");
            const LUID luid = device->GetAdapterLuid();
            gstDevice = gst_d3d12_device_new_for_adapter_luid(gst_d3d12_luid_to_int64(&luid));
            if (!gstDevice) throw std::runtime_error("Cannot create GStreamer D3D12 device on SDL's adapter");
            context = gst_d3d12_context_new(gstDevice);
            if (!context) throw std::runtime_error("Cannot create D3D12 GstContext");
            check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create copy fence");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) throw std::runtime_error("Cannot create fence event");
            ready = true;
            error.clear();
        } catch (const std::exception& e) { error = e.what(); }
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(instancesMutex());
            auto& list = instances();
            list.erase(std::remove(list.begin(), list.end(), this), list.end());
        }
        clearTextures();
        if (context) gst_context_unref(context);
        if (gstDevice) gst_object_unref(gstDevice);
        if (event) CloseHandle(event);
    }
    void retire() {
        const UINT64 completed = fence ? fence->GetCompletedValue() : 0;
        if (completed == UINT64_MAX) {
            if (!failedSubmission) {
                const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
                std::ostringstream msg;
                msg << "D3D12 copy fence reports device removal (0x"
                    << std::hex << static_cast<unsigned long>(reason) << ")";
                error = msg.str();
                LOG_ERROR("D3D12VideoInterop", error);
            }
            failedSubmission = true;
            current = nullptr;
            invalidate();
            return;
        }
        for (auto& slot : slots) {
            if (slot.flight && completed >= slot.flight) {
                slot.flight = 0;
                slot.sample.reset();
                slot.nativeOwner.reset();
                slot.source.Reset();
                slot.producerFence.Reset();
                slot.producerValue = 0;
                slot.planes = {};
                slot.cropX = slot.cropY = slot.cropW = slot.cropH = 0;
                slot.native = false;
            }
        }
    }
    void invalidate() {
        for (auto& slot : slots) if (slot.pending) {
            slot.pending = false;
            slot.sample.reset();
            slot.nativeOwner.reset();
            slot.source.Reset();
            slot.producerFence.Reset();
            slot.producerValue = 0;
            slot.planes = {};
            slot.cropX = slot.cropY = slot.cropW = slot.cropH = 0;
            slot.native = false;
        }
        current = nullptr;
    }
    void discard() {
        // Retargeting only cancels unsubmitted work. Teardown also drains copies.
        invalidate();
        for (auto& slot : slots) if (slot.flight && fence->GetCompletedValue() < slot.flight) {
            const HRESULT hr = fence->SetEventOnCompletion(slot.flight, event);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            if (SUCCEEDED(hr)) {
                while (fence->GetCompletedValue() < slot.flight) {
                    WaitForSingleObject(event, 50);
                    if (FAILED(device->GetDeviceRemovedReason())) break;
                    if (std::chrono::steady_clock::now() >= deadline) {
                        LOG_WARNING("D3D12VideoInterop", "discard() timed out waiting for copy fence completion; breaking wait");
                        break;
                    }
                }
            } else {
                // Event allocation failure does not imply GPU completion.
                while (fence->GetCompletedValue() < slot.flight &&
                    SUCCEEDED(device->GetDeviceRemovedReason())) {
                    Sleep(1);
                    if (std::chrono::steady_clock::now() >= deadline) {
                        LOG_WARNING("D3D12VideoInterop", "discard() timed out waiting for copy fence completion; breaking wait");
                        break;
                    }
                }
            }
        }
        retire();
        current = nullptr; // allocations remain warm, presentation does not
    }
    void clearTextures() {
        discard();
        fenceCache.clear();
        for (auto& slot : slots) {
            if (slot.texture) SDL_DestroyTexture(slot.texture);
            slot = Slot{};
        }
        width = height = 0;
    }
    void allocate(int w, int h, SDL_Colorspace c) {
        if (w == width && h == height && c == color) return;
        clearTextures();
        const int pitch = (w + 1) & ~1;
        const int rows = (h + 1) & ~1;
        std::vector<Uint8> black(static_cast<size_t>(pitch) * rows, 16);
        std::vector<Uint8> chroma(static_cast<size_t>(pitch) * (rows / 2), 128);
        for (auto& slot : slots) {
            const auto props = SDL_CreateProperties();
            checkSDL(props != 0, "Create SDL texture properties");
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_NV12);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, c);
            slot.texture = SDL_CreateTextureWithProperties(renderer, props);
            SDL_DestroyProperties(props);
            checkSDL(slot.texture != nullptr, "Create SDL NV12 ring texture");
            slot.destination = static_cast<ID3D12Resource*>(SDL_GetPointerProperty(
                SDL_GetTextureProperties(slot.texture), SDL_PROP_TEXTURE_D3D12_TEXTURE_POINTER, nullptr));
            if (!slot.destination) throw std::runtime_error("SDL NV12 texture has no D3D12 resource");

            const auto destinationDesc = slot.destination->GetDesc();
            const UINT64 expectedW = static_cast<UINT64>((w + 1) & ~1);
            const UINT expectedH = static_cast<UINT>((h + 1) & ~1);
            if (destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                destinationDesc.Format != DXGI_FORMAT_NV12 ||
                (destinationDesc.Width != static_cast<UINT64>(w) && destinationDesc.Width != expectedW) ||
                (destinationDesc.Height != static_cast<UINT>(h) && destinationDesc.Height != expectedH) ||
                destinationDesc.DepthOrArraySize != 1 ||
                destinationDesc.MipLevels != 1 ||
                destinationDesc.SampleDesc.Count != 1)
                throw std::runtime_error("SDL NV12 texture has unexpected D3D12 resource layout");

            // Establish SDL's internal state as PIXEL_SHADER_RESOURCE through
            // a public API. This upload happens once per allocation, not per frame.
            checkSDL(SDL_UpdateNVTexture(slot.texture, nullptr, black.data(), pitch, chroma.data(), pitch),
                "Initialize SDL texture state");
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&slot.allocator)), "Create copy allocator");
            check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.commands)), "Create copy list");
            check(slot.commands->Close(), "Close initial copy list");
        }
        // SDL 3.4.12 FlushRenderer only records its D3D12 list. This one-pixel
        // readback forces submission/completion of the initialization uploads.
        // No per-frame readback or upload is used. Never assume FlushRenderer
        // is a native-queue submission boundary.
        const SDL_Rect pixel{0, 0, 1, 1};
        SDL_Surface* probe = SDL_RenderReadPixels(renderer, &pixel);
        checkSDL(probe != nullptr, "Submit initial SDL texture state");
        SDL_DestroySurface(probe);
        width = w; height = h; color = c;
        LOG_INFO("D3D12VideoInterop", "Allocated NV12 ring; completed one-time SDL state initialization");
    }
    ComPtr<ID3D12Resource> importResource(GstD3D12Memory* memory) {
        auto* resource = gst_d3d12_memory_get_resource_handle(memory);
        auto* sourceDevice = gst_d3d12_device_get_device_handle(memory->device);
        if (sourceDevice == device.Get()) return resource;
        const LUID sourceLuid = sourceDevice->GetAdapterLuid();
        const LUID destLuid = device->GetAdapterLuid();
        if (sourceLuid.HighPart != destLuid.HighPart || sourceLuid.LowPart != destLuid.LowPart)
            throw std::runtime_error("Decoder and renderer use different adapters");
        // Cache the opened resource on the pool memory, not by a recyclable
        // pointer or NT handle. GStreamer destroys this when the memory dies.
        using Imported = ComPtr<ID3D12Resource>;
        auto* cached = static_cast<Imported*>(gst_d3d12_memory_get_token_data(memory, resourceToken));
        if (!cached) {
            HANDLE handle = nullptr; // borrowed; never CloseHandle this
            if (!gst_d3d12_memory_get_nt_handle(memory, &handle))
                throw std::runtime_error("Decoder resource is not shareable");
            auto imported = std::make_unique<Imported>();
            check(device->OpenSharedHandle(handle, IID_PPV_ARGS(imported->GetAddressOf())), "Import decoder resource");
            cached = imported.release();
            gst_d3d12_memory_set_token_data(memory, resourceToken, cached,
                [](gpointer p) { delete static_cast<Imported*>(p); });
        }
        return *cached;
    }
    ComPtr<ID3D12Fence> importFence(ID3D12Fence* producer) {
        ComPtr<ID3D12Device> owner;
        check(producer->GetDevice(IID_PPV_ARGS(&owner)), "Get producer fence device");
        if (owner.Get() == device.Get()) return producer;
        auto it = fenceCache.find(producer);
        if (it != fenceCache.end()) return it->second.imported;
        HANDLE handle = nullptr;
        check(owner->CreateSharedHandle(producer, nullptr, GENERIC_ALL, nullptr, &handle), "Share producer fence");
        ComPtr<ID3D12Fence> imported;
        const HRESULT hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&imported));
        CloseHandle(handle);
        check(hr, "Import producer fence");
        if (fenceCache.size() >= 32) fenceCache.clear();
        fenceCache[producer] = CachedFence{producer, imported};
        return imported;
    }
    SDL_Texture* prepare(GstSample* sample) {
        deferred = false;
        if (!ready || failedSubmission) return nullptr;
        retire();
        auto* buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info{};
        if (!buffer || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
            GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 || gst_buffer_n_memory(buffer) != 1)
            throw std::runtime_error("Expected single-resource D3D12 NV12 output");
        auto* memory = gst_buffer_peek_memory(buffer, 0);
        if (!gst_is_d3d12_memory(memory)) throw std::runtime_error("Decoder output is system memory");
        int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
        if (auto* c = gst_buffer_get_video_crop_meta(buffer)) {
            cropX = static_cast<int>(c->x);
            cropY = static_cast<int>(c->y);
            cropW = static_cast<int>(c->width);
            cropH = static_cast<int>(c->height);
        }
        auto* dmem = GST_D3D12_MEMORY_CAST(memory);
        auto source = importResource(dmem);
        const auto desc = source->GetDesc();
        // GStreamer 1.28 exports simultaneous-access output textures in COMMON.
        // Use implicit read promotion/decay; never transition a decoder reference
        // surface while its video queue may also be reading it.
        if (desc.Format != DXGI_FORMAT_NV12 ||
            !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) ||
            (desc.Flags & D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY))
            throw std::runtime_error("Unsupported decoder output resource flags/format");
        const int w = cropW > 0 ? cropW : GST_VIDEO_INFO_WIDTH(&info);
        const int h = cropH > 0 ? cropH : GST_VIDEO_INFO_HEIGHT(&info);
        if (w <= 0 || h <= 0 || UINT64((cropX + w + 1) & ~1) > desc.Width || UINT((cropY + h + 1) & ~1) > desc.Height)
            throw std::runtime_error("NV12 dimensions exceed decoder allocation");
        allocate(w, h, colorspace(info));
        Slot* selected = nullptr;
        // Multiple updates before rendering replace the pending frame rather
        // than retaining more decoder surfaces or growing the ring.
        for (auto& slot : slots) if (slot.pending) { selected = &slot; break; }
        if (!selected) for (auto& slot : slots) if (!slot.flight) { selected = &slot; break; }
        if (!selected) { deferred = true; return nullptr; }
        for (guint plane = 0; plane < 2; ++plane)
            if (!gst_d3d12_memory_get_subresource_index(dmem, plane, &selected->planes[plane]))
                throw std::runtime_error("Missing NV12 plane subresource");
        ComPtr<ID3D12Fence> producer;
        guint64 value = 0;
        gst_d3d12_memory_get_fence(dmem, &producer, &value); // transfer full
        selected->producerFence = producer ? importFence(producer.Get()) : nullptr;
        selected->producerValue = value;
        selected->source = source;
        selected->sample.reset(gst_sample_ref(sample));
        selected->nativeOwner.reset();
        selected->cropX = cropX;
        selected->cropY = cropY;
        selected->cropW = cropW;
        selected->cropH = cropH;
        selected->native = false;
        selected->pending = true;
        current = selected->texture;
        return current;
    }
    bool submit() {
        retire();
        if (failedSubmission) return true; // Inactive/degraded interop does not fail current frame
        try {
            for (auto& slot : slots) if (slot.pending) {
                if (slot.producerFence) {
                    const UINT64 completed = slot.producerFence->GetCompletedValue();
                    if (completed == UINT64_MAX) {
                        const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
                        std::ostringstream msg;
                        msg << "D3D12 producer fence reports device removal (0x"
                            << std::hex << static_cast<unsigned long>(reason) << ")";
                        throw std::runtime_error(msg.str());
                    }
                    if (completed < slot.producerValue)
                        continue;
                }

                check(slot.allocator->Reset(), "Reset copy allocator");
                check(slot.commands->Reset(slot.allocator.Get(), nullptr), "Reset copy list");

                // Source resource barrier:
                // Textures with ALLOW_SIMULTANEOUS_ACCESS promote/decay implicitly.
                // Non-simultaneous textures (e.g. FFmpeg decoder output) require explicit transitions
                // from COMMON to COPY_SOURCE and back to COMMON.
                const auto srcDesc = slot.source->GetDesc();
                const bool needSourceTransition =
                    !(srcDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);

                std::vector<D3D12_RESOURCE_BARRIER> beforeBarriers;
                beforeBarriers.push_back(transition(slot.destination,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST));
                if (needSourceTransition) {
                    for (UINT plane = 0; plane < 2; ++plane) {
                        beforeBarriers.push_back(transition(slot.source.Get(),
                            D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_STATE_COPY_SOURCE,
                            slot.planes[plane]));
                    }
                }
                slot.commands->ResourceBarrier(static_cast<UINT>(beforeBarriers.size()), beforeBarriers.data());

                const auto dstDesc = slot.destination->GetDesc();
                for (UINT plane = 0; plane < 2; ++plane) {
                    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                    src.pResource = slot.source.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = slot.planes[plane];
                    dst.pResource = slot.destination; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = plane;
                    const UINT divisor = plane ? 2 : 1;
                    const UINT srcX = (slot.cropW > 0 ? UINT(slot.cropX) : 0) / divisor;
                    const UINT srcY = (slot.cropH > 0 ? UINT(slot.cropY) : 0) / divisor;
                    const UINT maxSrcW = UINT(srcDesc.Width) / divisor;
                    const UINT maxSrcH = UINT(srcDesc.Height) / divisor;
                    const UINT maxDstW = UINT(dstDesc.Width) / divisor;
                    const UINT maxDstH = UINT(dstDesc.Height) / divisor;

                    UINT boxW = UINT((width + 1) & ~1) / divisor;
                    UINT boxH = UINT((height + 1) & ~1) / divisor;
                    if (srcX + boxW > maxSrcW) boxW = (maxSrcW > srcX) ? (maxSrcW - srcX) : 0;
                    if (srcY + boxH > maxSrcH) boxH = (maxSrcH > srcY) ? (maxSrcH - srcY) : 0;
                    if (boxW > maxDstW) boxW = maxDstW;
                    if (boxH > maxDstH) boxH = maxDstH;

                    const D3D12_BOX box{srcX, srcY, 0, srcX + boxW, srcY + boxH, 1};
                    const bool hasCropping = (slot.cropW > 0 || slot.cropH > 0);
                    const bool sameDimensions = (srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height);
                    const D3D12_BOX* pBox = (!hasCropping && sameDimensions) ? nullptr : &box;
                    slot.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, pBox);
                }

                std::vector<D3D12_RESOURCE_BARRIER> afterBarriers;
                afterBarriers.push_back(transition(slot.destination,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
                if (needSourceTransition) {
                    for (UINT plane = 0; plane < 2; ++plane) {
                        afterBarriers.push_back(transition(slot.source.Get(),
                            D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_COMMON,
                            slot.planes[plane]));
                    }
                }
                slot.commands->ResourceBarrier(static_cast<UINT>(afterBarriers.size()), afterBarriers.data());
                check(slot.commands->Close(), "Close copy list");

                // Cross-queue synchronization: ensure the direct queue waits on the producer fence
                if (slot.producerFence)
                    check(queue->Wait(slot.producerFence.Get(), slot.producerValue), "Queue producer fence wait");

                ID3D12CommandList* lists[]{slot.commands.Get()};
                queue->ExecuteCommandLists(1, lists);
                const HRESULT signal = queue->Signal(fence.Get(), ++sequence);
                if (FAILED(signal) && SUCCEEDED(device->GetDeviceRemovedReason())) {
                    LOG_ERROR("D3D12VideoInterop", "Copy fence signal failed on a live device; terminating to preserve GPU ownership");
                    std::terminate();
                }
                slot.flight = sequence;
                slot.pending = false;
                check(signal, "Signal copy completion");
                current = slot.texture;
            }
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            failedSubmission = true;
            current = nullptr;
            invalidate();
            const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
            if (FAILED(reason)) {
                std::ostringstream msg;
                msg << error << " (device removed reason: 0x" << std::hex << static_cast<unsigned long>(reason) << ")";
                error = msg.str();
            }
            LOG_ERROR("D3D12VideoInterop", error);
            return FAILED(reason) ? false : true;
        }
    }
};

D3D12VideoInterop::D3D12VideoInterop(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(renderer)) {}
D3D12VideoInterop::~D3D12VideoInterop() = default;
bool D3D12VideoInterop::available() const { return impl_->ready && !impl_->failedSubmission; }
const char* D3D12VideoInterop::reason() const { return impl_->error.c_str(); }
void D3D12VideoInterop::discardFrames() {
    SDL_assert(SDL_IsMainThread());
    impl_->discard();
}
void D3D12VideoInterop::invalidateFrame() {
    SDL_assert(SDL_IsMainThread());
    impl_->invalidate();
}
bool D3D12VideoInterop::deferred() const { return impl_->deferred; }
SDL_Texture* D3D12VideoInterop::currentTexture() const { return available() ? impl_->current : nullptr; }
SDL_Texture* D3D12VideoInterop::copy(GstSample* sample) {
    SDL_assert(SDL_IsMainThread());
    try { return impl_->prepare(sample); }
    catch (const std::exception& e) { impl_->error = e.what(); return nullptr; }
}
bool D3D12VideoInterop::beginFrame(SDL_Renderer* renderer) {
    SDL_assert(SDL_IsMainThread());
    bool ok = true;
    std::vector<Impl*> toSubmit;
    {
        std::lock_guard<std::mutex> lock(Impl::instancesMutex());
        for (auto* instance : Impl::instances())
            if (instance->renderer == renderer) toSubmit.push_back(instance);
    }
    for (auto* instance : toSubmit)
        ok = instance->submit() && ok;
    return ok;
}
SDL_Texture* D3D12VideoInterop::copyNative(ID3D12Resource* resource, ID3D12Fence* producer,
    uint64_t value, unsigned yPlane, unsigned uvPlane, int width, int height,
    SDL_Colorspace color, std::shared_ptr<void> owner,
    int cropX, int cropY, int cropW, int cropH) {
    SDL_assert(SDL_IsMainThread());
    auto& p = *impl_;
    p.deferred = false;
    if (!available()) return nullptr;

    try {
        p.retire();
        if (!available()) return nullptr;

        if (!resource || !owner)
            throw std::runtime_error("Missing native frame ownership");
        if (!producer)
            throw std::runtime_error("Native D3D12 frame has no producer fence");

        ComPtr<ID3D12Device> resourceDevice;
        check(resource->GetDevice(IID_PPV_ARGS(&resourceDevice)), "Get native frame device");
        if (resourceDevice.Get() != p.device.Get())
            throw std::runtime_error("Native D3D12 frame belongs to a different device");

        ComPtr<ID3D12Device> fenceDevice;
        check(producer->GetDevice(IID_PPV_ARGS(&fenceDevice)), "Get native producer fence device");
        if (fenceDevice.Get() != p.device.Get())
            throw std::runtime_error("Native D3D12 producer fence belongs to a different device");

        const auto desc = resource->GetDesc();
        // FFmpeg native frames do not need ALLOW_SIMULTANEOUS_ACCESS here.
        // The producer fence is the handoff: once reached, FFmpeg has returned
        // the decode output to COMMON and this queue may read it as COPY_SOURCE.
        const int w = cropW > 0 ? cropW : width;
        const int h = cropH > 0 ? cropH : height;
        const int effectiveCropX = cropW > 0 ? cropX : 0;
        const int effectiveCropY = cropH > 0 ? cropY : 0;

        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Format != DXGI_FORMAT_NV12 ||
            desc.SampleDesc.Count != 1 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY) ||
            w <= 0 || h <= 0 || effectiveCropX < 0 || effectiveCropY < 0 ||
            UINT64(effectiveCropX + w) > desc.Width ||
            UINT(effectiveCropY + h) > desc.Height)
            throw std::runtime_error("Incompatible native NV12 resource");

        const UINT planeStride = UINT(desc.MipLevels) * desc.DepthOrArraySize;
        if (!desc.MipLevels || !desc.DepthOrArraySize ||
            yPlane >= planeStride ||
            (yPlane % UINT(desc.MipLevels)) != 0 ||
            uvPlane != yPlane + planeStride)
            throw std::runtime_error("Invalid native NV12 subresource indices");

        p.allocate(w, h, color);

        // Multi-slot ring selection:
        // Look for an idle slot that is neither in-flight nor pending.
        Impl::Slot* selected = nullptr;
        for (auto& slot : p.slots) {
            if (!slot.flight && !slot.pending) {
                selected = &slot;
                break;
            }
        }

        if (!selected) {
            // All slots are active or pending. Defer rather than overwriting
            // an unsubmitted frame and dropping reference frame ownership.
            p.deferred = true;
            return p.current;
        }

        selected->sample.reset();
        selected->nativeOwner = std::move(owner);
        selected->source = resource;
        selected->producerFence = producer;
        selected->producerValue = value;
        selected->planes = {yPlane, uvPlane};
        selected->cropX = effectiveCropX;
        selected->cropY = effectiveCropY;
        selected->cropW = cropW > 0 ? cropW : 0;
        selected->cropH = cropH > 0 ? cropH : 0;
        selected->native = true;
        selected->pending = true;

        // No presentation exists until beginFrame submits the first copy.
        // Returning null here is deferred work, not an import failure.
        p.deferred = !p.current;
        return p.current;
    } catch (const std::exception& e) {
        p.error = e.what();
        return nullptr;
    }
}
void D3D12VideoInterop::configure(GstElement* pipeline) {
    if (!available()) return;
    gst_element_set_context(pipeline, impl_->context);
    auto* bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, [](GstBus*, GstMessage* message, gpointer data) {
        const gchar* type = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_NEED_CONTEXT &&
            gst_message_parse_context_type(message, &type) &&
            g_strcmp0(type, GST_D3D12_DEVICE_HANDLE_CONTEXT_TYPE) == 0)
            gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), static_cast<GstContext*>(data));
        return GST_BUS_PASS;
    }, gst_context_ref(impl_->context), [](gpointer p) { gst_context_unref(static_cast<GstContext*>(p)); });
    gst_object_unref(bus);
}
