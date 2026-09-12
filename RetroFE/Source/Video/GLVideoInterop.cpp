#include "GLVideoInterop.h"
#include "../Utility/Log.h"
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#ifdef RETROFE_GST_GL_EGL
#include <gst/gl/egl/gstgldisplay_egl.h>
#endif
#ifdef RETROFE_GST_GL_X11
#include <gst/gl/x11/gstgldisplay_x11.h>
#endif
#include <array>
#include <string>
#include <vector>

namespace {
struct CurrentContext {
    SDL_Window* previousWindow = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous = SDL_GL_GetCurrentContext();
    bool valid;
    CurrentContext(SDL_Renderer* renderer, SDL_GLContext context)
        : valid(SDL_GL_MakeCurrent(SDL_GetRenderWindow(renderer), context)) {}
    ~CurrentContext() { if (valid) SDL_GL_MakeCurrent(previousWindow, previous); }
};
struct SharedContexts {
    GstContext* display;
    GstContext* app;
    ~SharedContexts() { gst_context_unref(display); gst_context_unref(app); }
};
}

struct GLVideoInterop::Impl {
    SDL_Renderer* renderer;
    SDL_GLContext native = nullptr;
    GstGLDisplay* display = nullptr;
    GstGLContext* wrapped = nullptr;
    bool ready = false;
    bool gles = false;
    bool direct = false;
    SDL_Texture* directTexture = nullptr;
    GstSample* directSample = nullptr;
    std::string error = "OpenGL/OpenGL ES renderer required";
    struct Slot { GLuint native = 0; SDL_Texture* texture = nullptr; };
    std::array<Slot, 3> slots{};
    struct Pending { GstSample* sample; GLsync fence; SDL_Texture* texture = nullptr; };
    std::vector<Pending> pending;
    GLuint framebuffer = 0;
    int width = 0, height = 0;
    unsigned next = 0;
    explicit Impl(SDL_Renderer* r) : renderer(r) {}
    // Called only after SDL's queued reads have been submitted. Keep the
    // producer buffer out of its pool until those reads finish on the GPU.
    void releaseDirect() {
        if (!directSample) return;
        auto* gl = wrapped->gl_vtable;
        GLsync fence = gl->FenceSync && gl->ClientWaitSync && gl->DeleteSync
            ? gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
        if (fence) { pending.push_back({directSample, fence, directTexture}); gl->Flush(); }
        else {
            gl->Finish();
            SDL_DestroyTexture(directTexture);
            gst_sample_unref(directSample);
        }
        directTexture = nullptr;
        directSample = nullptr;
    }
    void retire(bool wait) {
        auto* gl = wrapped->gl_vtable;
        if (wait && !pending.empty()) gl->Finish();
        for (auto it = pending.begin(); it != pending.end();) {
            const GLenum state = wait ? GL_ALREADY_SIGNALED : gl->ClientWaitSync(it->fence, 0, 0);
            if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED || state == GL_WAIT_FAILED) {
                if (state == GL_WAIT_FAILED) gl->Finish();
                gl->DeleteSync(it->fence);
                if (it->texture) SDL_DestroyTexture(it->texture);
                gst_sample_unref(it->sample);
                it = pending.erase(it);
            } else ++it;
        }
    }
    void clear() {
        SDL_FlushRenderer(renderer);
        releaseDirect();
        retire(true);
        for (auto& slot : slots) {
            if (slot.texture) SDL_DestroyTexture(slot.texture);
            if (slot.native) wrapped->gl_vtable->DeleteTextures(1, &slot.native);
            slot = {};
        }
        width = height = next = 0;
    }
    ~Impl() {
        if (wrapped) {
            CurrentContext current(renderer, native);
            if (current.valid && gst_gl_context_activate(wrapped, TRUE)) {
                clear();
                if (framebuffer) wrapped->gl_vtable->DeleteFramebuffers(1, &framebuffer);
                gst_gl_context_activate(wrapped, FALSE);
            }
            gst_object_unref(wrapped);
        }
        if (display) gst_object_unref(display);
    }
};

GLVideoInterop::GLVideoInterop(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(renderer)) {
    auto& p = *impl_;
    const std::string backend = SDL_GetRendererName(renderer);
    if (backend != "opengl" && backend != "opengles2") return;
    p.gles = backend == "opengles2";
    p.direct = g_strcmp0(g_getenv("RETROFE_GL_DIRECT"), "1") == 0;
    p.native = static_cast<SDL_GLContext>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), "retrofe.gl.context", nullptr));
    if (!p.native) { p.error = "SDL renderer GL context not captured"; return; }
    CurrentContext current(renderer, p.native);
    if (!current.valid) { p.error = SDL_GetError(); return; }
    GstGLPlatform platform = GST_GL_PLATFORM_NONE;
    guintptr handle = 0;
#ifdef RETROFE_GST_GL_EGL
    if ((handle = gst_gl_context_get_current_gl_context(GST_GL_PLATFORM_EGL))) {
        auto eglDisplay = SDL_EGL_GetCurrentDisplay();
        if (eglDisplay) {
            p.display = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(eglDisplay));
            platform = GST_GL_PLATFORM_EGL;
        }
    }
#endif
#ifdef RETROFE_GST_GL_X11
    if (!p.display && (handle = gst_gl_context_get_current_gl_context(GST_GL_PLATFORM_GLX))) {
        auto* xdisplay = static_cast<Display*>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(SDL_GetRenderWindow(renderer)), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        if (xdisplay) {
            p.display = GST_GL_DISPLAY(gst_gl_display_x11_new_with_display(xdisplay));
            platform = GST_GL_PLATFORM_GLX;
        }
    }
#endif
    if (!p.display) { p.error = "no supported EGL/GLX display for SDL context"; return; }
    auto api = gst_gl_context_get_current_gl_api(platform, nullptr, nullptr);
    gst_gl_display_filter_gl_api(p.display, api);
    p.wrapped = gst_gl_context_new_wrapped(p.display, handle, platform, api);
    p.error = "could not initialize GStreamer wrapper for SDL GL context";
    if (!p.wrapped || !gst_gl_context_activate(p.wrapped, TRUE)) return;
    GError* error = nullptr;
    const bool filled = gst_gl_context_fill_info(p.wrapped, &error);
    if (error) { p.error = error->message; g_error_free(error); }
    gst_gl_context_activate(p.wrapped, FALSE);
    if (!filled) return;
    auto* gl = p.wrapped->gl_vtable;
    if (!gl->GenFramebuffers || !gl->FramebufferTexture2D || !gl->CopyTexSubImage2D) {
        p.error = "framebuffer texture copying unsupported by GL context"; return;
    }
    for (const char* name : {"glupload", "glcolorconvert"}) {
        auto* factory = gst_element_factory_find(name);
        if (!factory) { p.error = std::string("missing GStreamer plugin: ") + name; return; }
        gst_object_unref(factory);
    }
    p.ready = true;
    LOG_INFO("GStreamerVideo", (gl->FenceSync && gl->ClientWaitSync && gl->DeleteSync
        ? "GL interop synchronization: GPU fences"
        : "GL interop synchronization: blocking completion (GL sync objects unavailable)"));
}
GLVideoInterop::~GLVideoInterop() = default;
bool GLVideoInterop::available() const { return impl_->ready; }
const char* GLVideoInterop::reason() const { return impl_->error.c_str(); }
const char* GLVideoInterop::description() const {
    return impl_->direct ? "OpenGL direct RGBA texture wrapping; no final GPU copy"
        : "OpenGL RGBA GPU copy to SDL3 texture";
}
void GLVideoInterop::discardFrames() {
    if (!available()) return;
    auto& p = *impl_;
    CurrentContext current(p.renderer, p.native);
    if (current.valid && gst_gl_context_activate(p.wrapped, TRUE)) {
        SDL_FlushRenderer(p.renderer);
        p.releaseDirect();
        p.retire(true);
        gst_gl_context_activate(p.wrapped, FALSE);
    }
}

void GLVideoInterop::configure(GstElement* pipeline) {
    if (!available()) return;
    auto* contexts = new SharedContexts{gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE),
        gst_context_new("gst.gl.app_context", TRUE)};
    gst_context_set_gl_display(contexts->display, impl_->display);
    gst_structure_set(gst_context_writable_structure(contexts->app), "context", GST_TYPE_GL_CONTEXT, impl_->wrapped, nullptr);
    gst_element_set_context(pipeline, contexts->display);
    gst_element_set_context(pipeline, contexts->app);
    auto* bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, [](GstBus*, GstMessage* message, gpointer data) {
        const gchar* type = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_NEED_CONTEXT && gst_message_parse_context_type(message, &type)) {
            auto* c = static_cast<SharedContexts*>(data);
            if (g_strcmp0(type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), c->display);
            else if (g_strcmp0(type, "gst.gl.app_context") == 0) gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), c->app);
        }
        return GST_BUS_PASS;
    }, contexts, [](gpointer data) { delete static_cast<SharedContexts*>(data); });
    gst_object_unref(bus);
}

GstElement* GLVideoInterop::wrapSink(GstElement* sink) {
    auto* bin = gst_bin_new(nullptr);
    auto* gpuInput = gst_element_factory_make("capsfilter", nullptr);
    auto* upload = gst_element_factory_make("glupload", nullptr);
    auto* convert = gst_element_factory_make("glcolorconvert", nullptr);
    if (!bin || !gpuInput || !upload || !convert) {
        if (bin) gst_object_unref(bin);
        if (gpuInput) gst_object_unref(gpuInput);
        if (upload) gst_object_unref(upload);
        if (convert) gst_object_unref(convert);
        return nullptr;
    }
    // Do not let playbin negotiate system memory merely because glupload can
    // upload it. Leave DRM formats/modifiers to the decoder and EGL importer.
    // Unsupported GPU paths use GStreamerVideo's existing CPU retry.
    auto* inputCaps = gst_caps_from_string(
        "video/x-raw(memory:DMABuf);video/x-raw(memory:GLMemory)");
    g_object_set(gpuInput, "caps", inputCaps, nullptr);
    gst_caps_unref(inputCaps);
    // Preserve the caller's sink if bin construction fails after parenting it.
    const bool floating = g_object_is_floating(sink);
    gst_object_ref(sink);
    gst_bin_add_many(GST_BIN(bin), gpuInput, upload, convert, sink, nullptr);
    auto* inputPad = gst_element_get_static_pad(gpuInput, "sink");
    auto* ghost = gst_ghost_pad_new("sink", inputPad);
    gst_object_unref(inputPad);
    auto* pad = gst_element_get_static_pad(upload, "sink");
    // Log actual memory negotiation before glupload, independently of GL output.
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        [](GstPad*, GstPadProbeInfo* info, gpointer) {
            auto* event = GST_PAD_PROBE_INFO_EVENT(info);
            if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
                GstCaps* caps = nullptr;
                gst_event_parse_caps(event, &caps);
                gchar* text = gst_caps_to_string(caps);
                LOG_INFO("GStreamerVideo", std::string("GL upload input caps: ") + text);
                g_free(text);
            }
            return GST_PAD_PROBE_OK;
        }, nullptr, nullptr);
    gst_object_unref(pad);
    if (!ghost || !gst_element_add_pad(bin, ghost) || !gst_element_link_many(gpuInput, upload, convert, sink, nullptr)) {
        if (ghost && !GST_OBJECT_PARENT(ghost)) gst_object_unref(ghost);
        gst_object_unref(bin);
        if (floating) g_object_force_floating(G_OBJECT(sink));
        return nullptr;
    }
    gst_object_unref(sink);
    LOG_INFO("GStreamerVideo", "GL input requires DMA-BUF or GLMemory; CPU upload fallback on negotiation failure");
    return bin;
}

SDL_Texture* GLVideoInterop::copy(GstSample* sample) {
    auto& p = *impl_;
    if (!available()) return nullptr;
    auto* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info{};
    auto* caps = gst_sample_get_caps(sample);
    p.error = "expected a shared RGBA GLMemory texture";
    if (!buffer || !caps || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA || gst_buffer_n_memory(buffer) != 1) return nullptr;
    auto* memory = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_gl_memory(memory)) return nullptr;
    auto* source = reinterpret_cast<GstGLMemory*>(memory);
    if (gst_gl_memory_get_texture_target(source) != GST_GL_TEXTURE_TARGET_2D ||
        !gst_gl_context_can_share(p.wrapped, source->mem.context)) return nullptr;
    CurrentContext current(p.renderer, p.native);
    if (!current.valid || !gst_gl_context_activate(p.wrapped, TRUE)) return nullptr;
    struct Deactivate { GstGLContext* context; ~Deactivate() { gst_gl_context_activate(context, FALSE); } } deactivate{p.wrapped};
    SDL_FlushRenderer(p.renderer);
    auto* gl = p.wrapped->gl_vtable;
    p.retire(p.pending.size() >= 4);
    if (auto* sync = gst_buffer_get_gl_sync_meta(buffer)) gst_gl_sync_meta_wait(sync, p.wrapped);
    else gst_gl_context_thread_add(source->mem.context, [](GstGLContext* c, gpointer) { c->gl_vtable->Finish(); }, nullptr);
    const int w = gst_gl_memory_get_texture_width(source), h = gst_gl_memory_get_texture_height(source);
    if (w <= 0 || h <= 0) return nullptr;
    if (p.direct) {
        auto props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
        SDL_SetNumberProperty(props, p.gles ? SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER
            : SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_NUMBER, gst_gl_memory_get_texture_id(source));
        auto* texture = SDL_CreateTextureWithProperties(p.renderer, props);
        SDL_DestroyProperties(props);
        if (texture) {
            p.releaseDirect();
            p.directSample = gst_sample_ref(sample);
            p.directTexture = texture;
            return texture;
        }
        LOG_WARNING("GStreamerVideo", std::string("Direct GL wrapping failed; using GPU copy: ") + SDL_GetError());
        p.releaseDirect();
        p.direct = false;
    }
    if (w != p.width || h != p.height) {
        p.clear();
        GLint oldTexture = 0, oldUnpack = 0;
        gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
        const bool pbo = gst_gl_context_check_gl_version(p.wrapped, static_cast<GstGLAPI>(GST_GL_API_OPENGL | GST_GL_API_OPENGL3), 2, 1) ||
            gst_gl_context_check_gl_version(p.wrapped, GST_GL_API_GLES2, 3, 0);
        if (pbo) { gl->GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpack); gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); }
        bool ok = true;
        for (auto& slot : p.slots) {
            gl->GenTextures(1, &slot.native);
            gl->BindTexture(GL_TEXTURE_2D, slot.native);
            gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (gl->GetError() != GL_NO_ERROR) { ok = false; break; }
        }
        gl->BindTexture(GL_TEXTURE_2D, oldTexture);
        if (pbo) gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, oldUnpack);
        if (ok) for (auto& slot : p.slots) {
            auto props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
            SDL_SetNumberProperty(props, p.gles ? SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER : SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_NUMBER, slot.native);
            slot.texture = SDL_CreateTextureWithProperties(p.renderer, props);
            SDL_DestroyProperties(props);
            if (!slot.texture) { ok = false; break; }
        }
        if (!ok) { p.error = "failed to create shared GL texture ring"; p.clear(); return nullptr; }
        p.width = w; p.height = h;
    }
    GLint oldTexture = 0, oldRead = 0, oldDraw = 0;
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    const bool separate = gl->BlitFramebuffer != nullptr;
    gl->GetIntegerv(separate ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &oldRead);
    if (separate) gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
    if (!p.framebuffer) gl->GenFramebuffers(1, &p.framebuffer);
    gl->BindFramebuffer(GL_FRAMEBUFFER, p.framebuffer);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gst_gl_memory_get_texture_id(source), 0);
    auto& slot = p.slots[p.next++ % p.slots.size()];
    bool ok = gl->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        gl->BindTexture(GL_TEXTURE_2D, slot.native);
        gl->CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        ok = gl->GetError() == GL_NO_ERROR;
    }
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    gl->BindTexture(GL_TEXTURE_2D, oldTexture);
    gl->BindFramebuffer(separate ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER, oldRead);
    if (separate) gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDraw);
    if (!ok) { gl->Finish(); p.error = "GL framebuffer copy failed"; return nullptr; }
    GLsync fence = gl->FenceSync && gl->ClientWaitSync && gl->DeleteSync ? gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
    if (fence) { p.pending.push_back({gst_sample_ref(sample), fence}); gl->Flush(); }
    else gl->Finish(); // Older GLES contexts: wait safely without CPU pixel transfers.
    return slot.texture;
}
