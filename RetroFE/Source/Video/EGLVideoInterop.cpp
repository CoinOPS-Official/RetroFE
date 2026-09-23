#include "EGLVideoInterop.h"
#include "../Utility/Log.h"
#include "../SDL.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/video/video-info-dma.h>
#include <array>
#include <climits>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void ensure(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Context {
    SDL_Window* oldWindow = SDL_GL_GetCurrentWindow();
    SDL_GLContext old = SDL_GL_GetCurrentContext();
    Context(SDL_Renderer* renderer, SDL_GLContext native) {
        ensure(SDL_GL_MakeCurrent(SDL_GetRenderWindow(renderer), native), "cannot activate SDL EGL context");
    }
    ~Context() { SDL_GL_MakeCurrent(oldWindow, old); }
};
GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader); throw std::runtime_error(log);
    }
    return shader;
}
// Import the storage described by VideoMeta, not a tightly packed interpretation
// of the caps. Plane starts may refer to the same FD or different memory objects.
struct Frame {
    GstVideoInfoDmaDrm drm{};
    int width = 0, height = 0;
    SDL_Rect crop{};
    std::string colorDefaults;
    std::vector<EGLint> attrs;
    explicit Frame(const EGLDmaBufFrame& f) {
        ensure(f.owner && f.width > 0 && f.height > 0 && f.planeCount > 0 && f.planeCount <= 4,
               "invalid native DMA-BUF frame");
        width = f.width; height = f.height; crop = f.crop;
        ensure(crop.x >= 0 && crop.y >= 0 && crop.w > 0 && crop.h > 0 &&
            int64_t(crop.x)+crop.w <= width && int64_t(crop.y)+crop.h <= height, "invalid native crop");
        drm.drm_fourcc = f.fourcc; drm.drm_modifier = f.planes[0].modifier;
        attrs = {EGL_WIDTH,width,EGL_HEIGHT,height,EGL_LINUX_DRM_FOURCC_EXT,static_cast<EGLint>(f.fourcc)};
        const EGLint fd[] = {EGL_DMA_BUF_PLANE0_FD_EXT,EGL_DMA_BUF_PLANE1_FD_EXT,EGL_DMA_BUF_PLANE2_FD_EXT,EGL_DMA_BUF_PLANE3_FD_EXT};
        const EGLint off[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGL_DMA_BUF_PLANE1_OFFSET_EXT,EGL_DMA_BUF_PLANE2_OFFSET_EXT,EGL_DMA_BUF_PLANE3_OFFSET_EXT};
        const EGLint pitch[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT,EGL_DMA_BUF_PLANE1_PITCH_EXT,EGL_DMA_BUF_PLANE2_PITCH_EXT,EGL_DMA_BUF_PLANE3_PITCH_EXT};
        const EGLint low[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
        const EGLint high[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
        for (int i=0;i<f.planeCount;++i) {
            const auto& p=f.planes[i];
            ensure(p.fd>=0 && p.offset>=0 && p.pitch>0 && p.modifier==drm.drm_modifier,
                   "invalid or mixed-modifier DMA-BUF planes");
            attrs.insert(attrs.end(),{fd[i],p.fd,off[i],p.offset,pitch[i],p.pitch,
                low[i],static_cast<EGLint>(p.modifier),high[i],static_cast<EGLint>(p.modifier>>32)});
        }
        attrs.insert(attrs.end(),{EGL_YUV_COLOR_SPACE_HINT_EXT,f.bt709?EGL_ITU_REC709_EXT:EGL_ITU_REC601_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT,f.fullRange?EGL_YUV_FULL_RANGE_EXT:EGL_YUV_NARROW_RANGE_EXT});
        if(f.chromaX>=0) attrs.insert(attrs.end(),{EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT,f.chromaX?EGL_YUV_CHROMA_SITING_0_5_EXT:EGL_YUV_CHROMA_SITING_0_EXT});
        if(f.chromaY>=0) attrs.insert(attrs.end(),{EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT,f.chromaY?EGL_YUV_CHROMA_SITING_0_5_EXT:EGL_YUV_CHROMA_SITING_0_EXT});
        attrs.push_back(EGL_NONE);
    }
    explicit Frame(GstSample* sample) {
        gst_video_info_dma_drm_init(&drm);
        ensure(gst_video_info_dma_drm_from_caps(&drm, gst_sample_get_caps(sample)), "invalid DMA_DRM caps");
        auto* buffer = gst_sample_get_buffer(sample);
        auto* meta = gst_buffer_get_video_meta(buffer);
        ensure(meta && meta->width && meta->height && meta->width <= INT_MAX && meta->height <= INT_MAX,
               "missing/invalid DMA-BUF allocation metadata");
        ensure(meta->n_planes > 0 && meta->n_planes <= 4, "EGL import supports at most four storage planes");
        width = meta->width; height = meta->height;
        crop = {0, 0, drm.vinfo.width, drm.vinfo.height};
        if (auto* c = gst_buffer_get_video_crop_meta(buffer)) {
            ensure(c->x <= INT_MAX && c->y <= INT_MAX && c->width <= INT_MAX && c->height <= INT_MAX, "crop overflow");
            crop = {static_cast<int>(c->x), static_cast<int>(c->y), static_cast<int>(c->width), static_cast<int>(c->height)};
        }
        ensure(crop.x >= 0 && crop.y >= 0 && crop.w > 0 && crop.h > 0 &&
               static_cast<uint64_t>(crop.x) + crop.w <= meta->width &&
               static_cast<uint64_t>(crop.y) + crop.h <= meta->height, "crop outside imported allocation");
        attrs = {EGL_WIDTH, static_cast<EGLint>(meta->width), EGL_HEIGHT, static_cast<EGLint>(meta->height),
                 EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(drm.drm_fourcc)};
        const EGLint fd[] = {EGL_DMA_BUF_PLANE0_FD_EXT,EGL_DMA_BUF_PLANE1_FD_EXT,EGL_DMA_BUF_PLANE2_FD_EXT,EGL_DMA_BUF_PLANE3_FD_EXT};
        const EGLint off[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGL_DMA_BUF_PLANE1_OFFSET_EXT,EGL_DMA_BUF_PLANE2_OFFSET_EXT,EGL_DMA_BUF_PLANE3_OFFSET_EXT};
        const EGLint pitch[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT,EGL_DMA_BUF_PLANE1_PITCH_EXT,EGL_DMA_BUF_PLANE2_PITCH_EXT,EGL_DMA_BUF_PLANE3_PITCH_EXT};
        const EGLint low[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
        const EGLint high[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
        for (guint i = 0; i < meta->n_planes; ++i) {
            guint index = 0, count = 0; gsize skip = 0, base = 0;
            ensure(gst_buffer_find_memory(buffer, meta->offset[i], 1, &index, &count, &skip), "invalid plane offset");
            auto* memory = gst_buffer_peek_memory(buffer, index);
            ensure(gst_is_dmabuf_memory(memory), "plane does not use DMA-BUF storage");
            gst_memory_get_sizes(memory, &base, nullptr);
            ensure(base <= INT_MAX && skip <= INT_MAX - base && meta->stride[i] > 0, "invalid plane pitch/FD offset");
            attrs.insert(attrs.end(), {fd[i],gst_dmabuf_memory_get_fd(memory),off[i],static_cast<EGLint>(base+skip),
                pitch[i],meta->stride[i],low[i],static_cast<EGLint>(drm.drm_modifier & 0xffffffffu),high[i],static_cast<EGLint>(drm.drm_modifier >> 32)});
        }
        GstVideoInfo ordinary{};
        ensure(gst_video_info_dma_drm_to_video_info(&drm, &ordinary), "unknown DRM pixel format");
        // RGBA8 is the SDR output contract. P010 SDR imports are allowed, but
        // reduced to 8 bits. Reject HDR rather than silently displaying it wrong.
        auto color = drm.vinfo.colorimetry;
        ensure(color.transfer != GST_VIDEO_TRANSFER_SMPTE2084 && color.transfer != GST_VIDEO_TRANSFER_ARIB_STD_B67,
               "HDR DMA-BUF requires tone mapping; selecting CPU fallback");
        if (GST_VIDEO_INFO_IS_YUV(&ordinary)) {
            // DMA_DRM caps can leave colorimetry unknown. Ask GStreamer for
            // defaults using the actual pixel format and coded dimensions,
            // not DMA_DRM or the padded allocation/cropped display size.
            GstVideoInfo defaults{};
            ensure(gst_video_info_set_format(&defaults, GST_VIDEO_INFO_FORMAT(&ordinary),
                       drm.vinfo.width, drm.vinfo.height), "cannot resolve default YUV colorimetry");
            std::string fields;
            if (color.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
                color.matrix = defaults.colorimetry.matrix; fields += " matrix";
            }
            if (color.range == GST_VIDEO_COLOR_RANGE_UNKNOWN) {
                color.range = defaults.colorimetry.range; fields += " range";
            }
            if (color.transfer == GST_VIDEO_TRANSFER_UNKNOWN) {
                color.transfer = defaults.colorimetry.transfer; fields += " transfer";
            }
            if (color.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) {
                color.primaries = defaults.colorimetry.primaries; fields += " primaries";
            }
            if (!fields.empty()) {
                gchar* resolved = gst_video_colorimetry_to_string(&color);
                colorDefaults = "EGL colorimetry defaults applied to unspecified fields:" + fields
                    + "; resolved=" + (resolved ? std::string(resolved) : "unknown")
                    + "; coded size=" + std::to_string(drm.vinfo.width) + "x" + std::to_string(drm.vinfo.height);
                g_free(resolved);
            }
            ensure(color.matrix == GST_VIDEO_COLOR_MATRIX_BT709 || color.matrix == GST_VIDEO_COLOR_MATRIX_BT601,
                   "unsupported YUV matrix (only SDR BT.601/709 supported)");
            ensure(color.range == GST_VIDEO_COLOR_RANGE_0_255 || color.range == GST_VIDEO_COLOR_RANGE_16_235, "unknown YUV range");
            attrs.insert(attrs.end(), {EGL_YUV_COLOR_SPACE_HINT_EXT,color.matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? EGL_ITU_REC709_EXT : EGL_ITU_REC601_EXT,
                EGL_SAMPLE_RANGE_HINT_EXT,color.range == GST_VIDEO_COLOR_RANGE_0_255 ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT});

            // EGL_EXT_image_dma_buf_import lets the importer know where chroma
            // samples sit relative to luma. GStreamer represents this as flags:
            // H_COSITED => horizontal position 0 instead of 0.5
            // V_COSITED => vertical position 0 instead of 0.5
            //
            // ALT_LINE (used by DV-style siting) alternates vertically between
            // lines and cannot be represented by EGL's single vertical siting
            // hint, so leave both hints unspecified in that case.
            const GstVideoChromaSite chromaSite = ordinary.chroma_site;
            if (chromaSite != GST_VIDEO_CHROMA_SITE_UNKNOWN &&
                !(chromaSite & GST_VIDEO_CHROMA_SITE_ALT_LINE)) {
                const EGLint horizontal =
                    (chromaSite & GST_VIDEO_CHROMA_SITE_H_COSITED)
                        ? EGL_YUV_CHROMA_SITING_0_EXT
                        : EGL_YUV_CHROMA_SITING_0_5_EXT;
                const EGLint vertical =
                    (chromaSite & GST_VIDEO_CHROMA_SITE_V_COSITED)
                        ? EGL_YUV_CHROMA_SITING_0_EXT
                        : EGL_YUV_CHROMA_SITING_0_5_EXT;

                attrs.insert(attrs.end(), {
                    EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, horizontal,
                    EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, vertical
                });
            }
        }
        attrs.push_back(EGL_NONE);
    }
};

using ModifierKey = std::pair<guint32, guint64>;
using ModifierCache = std::map<ModifierKey, bool>;

// EGL modifier support is a display/driver property, not a per-video property.
// EGLVideoInterop is main/render-thread only, so this cache needs no mutex.
std::map<EGLDisplay, ModifierCache> gModifierSupport;

bool queryModifierSupport(EGLDisplay display,
                          PFNEGLQUERYDMABUFMODIFIERSEXTPROC queryModifiers,
                          const ModifierKey& key)
{
    auto& cache = gModifierSupport[display];
    if (auto found = cache.find(key); found != cache.end())
        return found->second;

    EGLint count = 0;
    bool ok = queryModifiers(display, key.first, 0, nullptr, nullptr, &count);

    std::vector<EGLuint64KHR> modifiers(ok ? count : 0);
    std::vector<EGLBoolean> externalOnly(modifiers.size());

    if (ok && count) {
        ok = queryModifiers(display, key.first, count,
                            modifiers.data(), externalOnly.data(), &count);
    }

    bool match = false;
    if (ok) {
        for (EGLint i = 0; i < count; ++i) {
            if (modifiers[i] == key.second) {
                match = true;
                break;
            }
        }
    }

    // Keep a bounded process-local capability cache in case a driver exposes a
    // very large set of format/modifier combinations over the application's life.
    if (cache.size() >= 128)
        cache.clear();

    cache.emplace(key, match);
    return match;
}

struct GLStateGuard {
    GLint framebuffer = 0;
    GLint viewport[4]{};
    GLint program = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint texture2DUnit0 = 0;
    GLint externalTextureUnit0 = 0;

    GLint attribEnabled = 0;
    GLint attribSize = 4;
    GLint attribStride = 0;
    GLint attribType = GL_FLOAT;
    GLint attribNormalized = GL_FALSE;
    GLint attribBuffer = 0;
    void* attribPointer = nullptr;

    GLboolean scissor = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean depth = GL_FALSE;
    GLboolean stencil = GL_FALSE;
    GLboolean cull = GL_FALSE;
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};

    GLStateGuard()
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);

        scissor = glIsEnabled(GL_SCISSOR_TEST);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribEnabled);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribSize);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribStride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribType);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribNormalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribBuffer);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribPointer);

        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2DUnit0);
        glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &externalTextureUnit0);
        glActiveTexture(activeTexture);
    }

    ~GLStateGuard()
    {
        setEnabled(GL_SCISSOR_TEST, scissor);
        setEnabled(GL_BLEND, blend);
        setEnabled(GL_DEPTH_TEST, depth);
        setEnabled(GL_STENCIL_TEST, stencil);
        setEnabled(GL_CULL_FACE, cull);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);

        glUseProgram(static_cast<GLuint>(program));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2DUnit0));
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLuint>(externalTextureUnit0));
        glActiveTexture(activeTexture);

        // glVertexAttribPointer captures the ARRAY_BUFFER binding as part of the
        // attribute state, so restore that binding first, then the caller's
        // global ARRAY_BUFFER binding after restoring attrib 0.
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attribBuffer));
        glVertexAttribPointer(0, attribSize, static_cast<GLenum>(attribType),
                              attribNormalized ? GL_TRUE : GL_FALSE,
                              attribStride, attribPointer);
        if (attribEnabled)
            glEnableVertexAttribArray(0);
        else
            glDisableVertexAttribArray(0);

        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
    }

private:
    static void setEnabled(GLenum cap, GLboolean enabled)
    {
        if (enabled)
            glEnable(cap);
        else
            glDisable(cap);
    }
};
}

struct EGLVideoInterop::Impl {
    static constexpr size_t kIdleLimit = 2;
    static constexpr size_t kMaxPending = 4;

    SDL_Renderer* renderer;
    SDL_GLContext native = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC queryModifiers = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC waitSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;

    bool ready = false;
    std::string error = "EGL opengles2 renderer required";
    std::string lastColorDefaults;

    GLuint program = 0;
    GLuint vbo = 0;
    GLuint fbo = 0;
    GLuint output = 0;
    GLint videoUniform = -1;
    GLint cropUniform = -1;

    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;

    struct Pending {
        std::shared_ptr<void> sample;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        EGLSyncKHR fence = EGL_NO_SYNC_KHR;
        SDL_Texture* wrapper = nullptr;
        int width = 0;
        int height = 0;
    };

    Pending direct{};
    std::vector<Pending> pending;
    std::vector<Pending> idle;

    bool preferDirect = true;
    int visibleWidth = 0;
    int visibleHeight = 0;

    explicit Impl(SDL_Renderer* r) : renderer(r) {}

    // Only completed direct imports enter this cache. The GstSample and EGLImage
    // are always released before a texture/wrapper slot becomes reusable.
    void recycle(Pending& frame)
    {
        if (frame.image != EGL_NO_IMAGE_KHR)
            destroyImage(display, frame.image);
        frame.sample.reset();

        if (frame.wrapper && idle.size() < kIdleLimit) {
            idle.push_back({nullptr, EGL_NO_IMAGE_KHR, frame.texture,
                            EGL_NO_SYNC_KHR, frame.wrapper,
                            frame.width, frame.height});
        } else {
            if (frame.wrapper)
                SDL_DestroyTexture(frame.wrapper);
            if (frame.texture)
                glDeleteTextures(1, &frame.texture);
        }

        frame = {};
    }

    // Caller must have flushed SDL before this. The fence is inserted after
    // SDL's last queued read of the direct texture has been submitted.
    void releaseDirectAfterFlush()
    {
        if (!direct.sample)
            return;

        direct.fence = createSync(display, EGL_SYNC_FENCE_KHR, nullptr);
        if (direct.fence == EGL_NO_SYNC_KHR) {
            glFinish(); // recovery only
            recycle(direct);
        } else {
            pending.push_back(direct);
            direct = {};
            glFlush();
        }
    }

    void retireCompleted()
    {
        for (auto it = pending.begin(); it != pending.end();) {
            const EGLint result = waitSync(display, it->fence, 0, 0);
            if (result == EGL_TIMEOUT_EXPIRED_KHR) {
                ++it;
                continue;
            }

            if (result != EGL_CONDITION_SATISFIED_KHR)
                glFinish(); // failed wait recovery only

            destroySync(display, it->fence);
            it->fence = EGL_NO_SYNC_KHR;
            recycle(*it);
            it = pending.erase(it);
        }
    }

    void waitOldest()
    {
        if (pending.empty())
            return;

        auto& frame = pending.front();
        constexpr EGLuint64KHR timeoutNs = 500000000ULL; // 500ms bounded timeout
        const EGLint result = waitSync(display, frame.fence,
                                       EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                       timeoutNs);
        if (result != EGL_CONDITION_SATISFIED_KHR) {
            LOG_WARNING("EGLVideoInterop", "waitOldest() timed out or failed waiting for EGL sync fence; falling back to glFinish()");
            glFinish(); // failed wait recovery only
        }

        destroySync(display, frame.fence);
        frame.fence = EGL_NO_SYNC_KHR;
        recycle(frame);
        pending.erase(pending.begin());
    }

    void drainPending()
    {
        while (!pending.empty())
            waitOldest();
    }

    bool supports(const Frame& frame)
    {
        return queryModifierSupport(
            display, queryModifiers,
            {frame.drm.drm_fourcc, frame.drm.drm_modifier});
    }

    void resize(int w, int h)
    {
        if (texture && width == w && height == h)
            return;

        // The previous conversion texture may still be referenced by queued GPU
        // work. At a size transition, drain before replacing its storage.
        drainPending();

        if (texture)
            SDL_DestroyTexture(texture);
        texture = nullptr;

        if (output)
            glDeleteTextures(1, &output);
        output = 0;

        glGenTextures(1, &output);
        glBindTexture(GL_TEXTURE_2D, output);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        auto props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                              SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
        SDL_SetNumberProperty(props,
                              SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER,
                              output);
        texture = SDL_CreateTextureWithProperties(renderer, props);
        SDL_DestroyProperties(props);

        ensure(texture, "SDL EGL output wrapper creation failed");
        width = w;
        height = h;
    }

    ~Impl()
    {
        if (!native)
            return;

        try {
            Context current(renderer, native);

            // One flush establishes the SDL->raw-GL boundary. Everything below
            // operates on already-submitted SDL work.
            SDL_FlushRenderer(renderer);
            releaseDirectAfterFlush();
            drainPending();

            for (auto& slot : idle) {
                SDL_DestroyTexture(slot.wrapper);
                glDeleteTextures(1, &slot.texture);
            }
            idle.clear();

            if (texture)
                SDL_DestroyTexture(texture);

            // The GL object lifetime rules already make deleting an attached
            // texture safe, but detach explicitly so teardown leaves no stale
            // attachment relationship behind. Preserve whichever framebuffer
            // the caller had bound unless it happens to be this FBO itself.
            if (fbo) {
                GLint oldFramebuffer = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, 0, 0);
                glBindFramebuffer(GL_FRAMEBUFFER,
                                  oldFramebuffer == static_cast<GLint>(fbo)
                                      ? 0u
                                      : static_cast<GLuint>(oldFramebuffer));
            }

            if (output)
                glDeleteTextures(1, &output);
            if (fbo)
                glDeleteFramebuffers(1, &fbo);
            if (vbo)
                glDeleteBuffers(1, &vbo);
            if (program)
                glDeleteProgram(program);
        } catch (...) {
            LOG_ERROR("GStreamerVideo",
                      "EGL cleanup could not activate its renderer context");
        }
    }
};

EGLVideoInterop::EGLVideoInterop(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(renderer)) {
    auto& p=*impl_;
    const char* mode = std::getenv("RETROFE_EGL_DIRECT");
    p.preferDirect = !mode || std::string(mode) != "0";
    p.pending.reserve(Impl::kMaxPending + 1);
    p.idle.reserve(Impl::kIdleLimit);
    if (!renderer || std::string(SDL_GetRendererName(renderer)) != "opengles2") return;
    p.native=static_cast<SDL_GLContext>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),"retrofe.gl.context",nullptr));
    if (!p.native) return;
    try {
        Context current(renderer,p.native); SDL_FlushRenderer(renderer);
        p.display=static_cast<EGLDisplay>(SDL_EGL_GetCurrentDisplay());
        ensure(p.display != EGL_NO_DISPLAY,"no EGL display");
        const char* e=eglQueryString(p.display,EGL_EXTENSIONS);
        const std::string ext=std::string(" ")+(e?e:"")+" ";
        ensure(ext.find(" EGL_EXT_image_dma_buf_import_modifiers ")!=std::string::npos && ext.find(" EGL_KHR_fence_sync ")!=std::string::npos,"EGL modifier import/fence extensions missing");
#define LOAD(member, type, name) p.member=reinterpret_cast<type>(eglGetProcAddress(name)); ensure(p.member,name)
        LOAD(createImage,PFNEGLCREATEIMAGEKHRPROC,"eglCreateImageKHR");
        LOAD(destroyImage,PFNEGLDESTROYIMAGEKHRPROC,"eglDestroyImageKHR");
        LOAD(imageTarget,PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,"glEGLImageTargetTexture2DOES");
        LOAD(queryModifiers,PFNEGLQUERYDMABUFMODIFIERSEXTPROC,"eglQueryDmaBufModifiersEXT");
        LOAD(createSync,PFNEGLCREATESYNCKHRPROC,"eglCreateSyncKHR");
        LOAD(waitSync,PFNEGLCLIENTWAITSYNCKHRPROC,"eglClientWaitSyncKHR");
        LOAD(destroySync,PFNEGLDESTROYSYNCKHRPROC,"eglDestroySyncKHR");
#undef LOAD
        GLuint vertex=compile(GL_VERTEX_SHADER,"attribute vec2 pos; uniform vec4 crop; varying vec2 uv; void main(){uv=crop.xy+(pos+1.0)*0.5*crop.zw;gl_Position=vec4(pos,0.,1.);}");
        GLuint fragment=0;
        try { fragment=compile(GL_FRAGMENT_SHADER,"#extension GL_OES_EGL_image_external : require\nprecision mediump float; varying vec2 uv; uniform samplerExternalOES video; void main(){gl_FragColor=vec4(texture2D(video,uv).rgb,1.);}"); }
        catch (...) { glDeleteShader(vertex); throw; }
        p.program=glCreateProgram(); glAttachShader(p.program,vertex); glAttachShader(p.program,fragment);
        glBindAttribLocation(p.program,0,"pos"); glLinkProgram(p.program); glDeleteShader(vertex); glDeleteShader(fragment);
        GLint ok=0; glGetProgramiv(p.program,GL_LINK_STATUS,&ok); ensure(ok,"EGL conversion shader link failed");
        p.videoUniform = glGetUniformLocation(p.program, "video");
        p.cropUniform = glGetUniformLocation(p.program, "crop");
        ensure(p.videoUniform >= 0 && p.cropUniform >= 0, "EGL conversion shader uniforms missing");

        // The external-video sampler is invariant: conversion always samples
        // GL_TEXTURE0. Set it once instead of updating the uniform every frame.
        GLint oldProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glUseProgram(p.program);
        glUniform1i(p.videoUniform, 0);
        glUseProgram(static_cast<GLuint>(oldProgram));

        const GLfloat quad[]={-1,-1,1,-1,-1,1,1,1};
        GLint oldBuffer=0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&oldBuffer);
        glGenBuffers(1,&p.vbo); glBindBuffer(GL_ARRAY_BUFFER,p.vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER,oldBuffer); glGenFramebuffers(1,&p.fbo);
        p.ready=true;
    } catch (const std::exception& e) { p.error=e.what(); }
}
EGLVideoInterop::~EGLVideoInterop()=default;
bool EGLVideoInterop::available() const { return impl_->ready; }
const char* EGLVideoInterop::description() const {
    return impl_->direct.sample
        ? "EGL DMA-BUF direct SDL external texture; GPU fences; no RGBA intermediate; no GStreamer GL context"
        : "EGL DMA-BUF conversion to reusable SDL RGBA texture; GPU fences; no GStreamer GL context";
}
const char* EGLVideoInterop::reason() const { return impl_->error.c_str(); }
int EGLVideoInterop::width() const { return impl_->visibleWidth; }
int EGLVideoInterop::height() const { return impl_->visibleHeight; }
GstElement* EGLVideoInterop::wrapSink(GstElement* sink) {
    return sink;
}
bool EGLVideoInterop::proposeAllocation(GstQuery* query) {
    if (!gst_query_find_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr))
        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
    if (!gst_query_find_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, nullptr))
        gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, nullptr);
    return true;
}
void EGLVideoInterop::discardFrames() noexcept {
    auto& p = *impl_;
    if (!p.ready) return;
    p.lastColorDefaults.clear();
    try {
        if (!SDL_FlushRenderer(p.renderer)) {
            LOG_WARNING("EGLVideoInterop", "SDL_FlushRenderer failed during discardFrames: " + std::string(SDL_GetError()));
        }
        Context current(p.renderer, p.native);
        p.releaseDirectAfterFlush();
        p.drainPending();
    } catch (const std::exception& e) {
        LOG_WARNING("EGLVideoInterop", "discardFrames exception: " + std::string(e.what()));
    }
    // Keep output, wrapper, FBO and shader for compatible future media.
}
SDL_Texture* EGLVideoInterop::copy(GstSample* sample) { return copyFrame(sample, nullptr); }
SDL_Texture* EGLVideoInterop::copy(const EGLDmaBufFrame& frame) { return copyFrame(nullptr, &frame); }
SDL_Texture* EGLVideoInterop::copyFrame(GstSample* sample, const EGLDmaBufFrame* native) {
    auto& p=*impl_; if (!p.ready) return nullptr;
    EGLImageKHR image=EGL_NO_IMAGE_KHR; GLuint input=0; SDL_Texture* reusableWrapper=nullptr;
    try {
        Frame frame = native ? Frame(*native) : Frame(sample);
        auto owner = native ? native->owner : std::shared_ptr<void>(gst_sample_ref(sample), [](void* p) { gst_sample_unref(static_cast<GstSample*>(p)); });
        if (frame.colorDefaults != p.lastColorDefaults) {
            if (!frame.colorDefaults.empty()) LOG_INFO("GStreamerVideo", frame.colorDefaults);
            p.lastColorDefaults = frame.colorDefaults;
        }
        Context current(p.renderer,p.native);
        ensure(p.supports(frame),"EGL does not advertise this DRM format/modifier");

        // SDL3 always batches renderer work. Flush exactly once before touching
        // the underlying GLES context directly. Coalesced per frame across videos.
        ensure(SDL::flushVideoRenderer(p.renderer), "SDL_FlushRenderer failed");

        // Everything below this point is raw EGL/GL work. Snapshot SDL's GL
        // state immediately after flushing its batch so the whole interop
        // operation is transactional, including fence retirement.
        GLStateGuard restore;

        p.releaseDirectAfterFlush();
        p.retireCompleted();
        if (p.pending.size() >= Impl::kMaxPending)
            p.waitOldest();

        image=p.createImage(p.display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,frame.attrs.data());
        ensure(image!=EGL_NO_IMAGE_KHR,"EGL DMA-BUF image import failed");
        // A direct wrapper exposes the whole image. Use conversion for crop or
        // padded visible dimensions until the render API carries a source rect.
        const bool wholeImage = frame.crop.x == 0 && frame.crop.y == 0 &&
            frame.crop.w == frame.width &&
            frame.crop.h == frame.height;
        const bool tryDirect = p.preferDirect && wholeImage && frame.drm.drm_fourcc == 0x3231564e;
        if (tryDirect && !p.idle.empty()) {
            size_t selected = p.idle.size()-1;
            for (size_t i=0; i<p.idle.size(); ++i)
                if (p.idle[i].width == frame.crop.w && p.idle[i].height == frame.crop.h) { selected=i; break; }
            auto slot=p.idle[selected];
            p.idle.erase(p.idle.begin()+selected);
            input=slot.texture;
            if (slot.width == frame.crop.w && slot.height == frame.crop.h) reusableWrapper=slot.wrapper;
            else SDL_DestroyTexture(slot.wrapper); // dimensions are part of SDL's wrapper
        }
        glActiveTexture(GL_TEXTURE0); if (!input) glGenTextures(1,&input); glBindTexture(GL_TEXTURE_EXTERNAL_OES,input);
        p.imageTarget(GL_TEXTURE_EXTERNAL_OES,image);
        // Preserve sampler settings that SDL caches on a reused wrapper.
        if (!reusableWrapper) {
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        ensure(glGetError()==GL_NO_ERROR, "EGL external texture rebind failed");
        if (tryDirect) {
            auto* wrapper = reusableWrapper;
            reusableWrapper = nullptr;
            if (!wrapper) {
                auto props = SDL_CreateProperties();
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_EXTERNAL_OES);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame.crop.w);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame.crop.h);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER, input);
                wrapper = SDL_CreateTextureWithProperties(p.renderer, props);
                SDL_DestroyProperties(props);
                if (wrapper) LOG_DEBUG("GStreamerVideo", "Created reusable EGL direct texture slot " + std::to_string(frame.crop.w) + "x" + std::to_string(frame.crop.h));
            }
            if (wrapper) {
                p.direct = {owner, image, input, EGL_NO_SYNC_KHR, wrapper, frame.crop.w, frame.crop.h};
                image = EGL_NO_IMAGE_KHR; input = 0;
                p.visibleWidth = frame.crop.w; p.visibleHeight = frame.crop.h;
                return wrapper;
            }
            LOG_INFO("GStreamerVideo", std::string("SDL external texture unavailable; using EGL RGBA conversion: ") + SDL_GetError());
            p.preferDirect = false; // renderer capability failure; avoid per-frame retries
            // Clear errors from the failed wrapper attempt before conversion.
            while (glGetError() != GL_NO_ERROR) {}
        }
        p.resize(frame.crop.w,frame.crop.h);
        p.visibleWidth = frame.crop.w; p.visibleHeight = frame.crop.h;
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES,input);
        glBindFramebuffer(GL_FRAMEBUFFER,p.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,p.output,0);
        ensure(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"EGL conversion FBO incomplete");
        glViewport(0,0,p.width,p.height); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glUseProgram(p.program);
        glUniform4f(p.cropUniform,float(frame.crop.x)/frame.width,float(frame.crop.y)/frame.height,
                    float(frame.crop.w)/frame.width,float(frame.crop.h)/frame.height);
        glBindBuffer(GL_ARRAY_BUFFER,p.vbo); glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4); ensure(glGetError()==GL_NO_ERROR,"EGL external conversion GL error");
        auto fence=p.createSync(p.display,EGL_SYNC_FENCE_KHR,nullptr);
        ensure(fence!=EGL_NO_SYNC_KHR,"EGL conversion fence creation failed");
        p.pending.push_back({owner,image,input,fence}); image=EGL_NO_IMAGE_KHR; input=0;
        glFlush(); return p.texture;
    } catch (const std::exception& e) {
        p.error=e.what();
        // Preserve the exact negotiated metadata: an unknown matrix and an
        // explicitly unsupported matrix must be distinguishable in reports.
        if (auto* caps = sample ? gst_sample_get_caps(sample) : nullptr) {
            gchar* text = gst_caps_to_string(caps);
            p.error += std::string("; negotiated caps: ") + (text ? text : "unavailable");
            g_free(text);
            GstVideoInfoDmaDrm info{};
            gst_video_info_dma_drm_init(&info);
            if (gst_video_info_dma_drm_from_caps(&info, caps)) {
                const auto& color = info.vinfo.colorimetry;
                p.error += "; parsed colorimetry matrix=" + std::to_string(color.matrix)
                    + " range=" + std::to_string(color.range)
                    + " transfer=" + std::to_string(color.transfer)
                    + " primaries=" + std::to_string(color.primaries);
            }
        }
        // Release imported storage only after any partially issued GPU work.
        try { Context current(p.renderer,p.native); glFinish(); if(reusableWrapper)SDL_DestroyTexture(reusableWrapper); if(input)glDeleteTextures(1,&input); if(image!=EGL_NO_IMAGE_KHR)p.destroyImage(p.display,image); } catch (...) {}
        return nullptr;
    }
}
