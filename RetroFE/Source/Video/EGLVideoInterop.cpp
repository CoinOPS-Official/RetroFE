#include "EGLVideoInterop.h"
#include "../Utility/Log.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/video/video-info-dma.h>
#include <array>
#include <climits>
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
    SDL_Rect crop{};
    std::vector<EGLint> attrs;
    explicit Frame(GstSample* sample) {
        gst_video_info_dma_drm_init(&drm);
        ensure(gst_video_info_dma_drm_from_caps(&drm, gst_sample_get_caps(sample)), "invalid DMA_DRM caps");
        auto* buffer = gst_sample_get_buffer(sample);
        auto* meta = gst_buffer_get_video_meta(buffer);
        ensure(meta && meta->width && meta->height && meta->width <= INT_MAX && meta->height <= INT_MAX,
               "missing/invalid DMA-BUF allocation metadata");
        ensure(meta->n_planes > 0 && meta->n_planes <= 4, "EGL import supports at most four storage planes");
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
        const auto color = drm.vinfo.colorimetry;
        ensure(color.transfer != GST_VIDEO_TRANSFER_SMPTE2084 && color.transfer != GST_VIDEO_TRANSFER_ARIB_STD_B67,
               "HDR DMA-BUF requires tone mapping; selecting CPU fallback");
        if (GST_VIDEO_INFO_IS_YUV(&ordinary)) {
            ensure(color.matrix == GST_VIDEO_COLOR_MATRIX_BT709 || color.matrix == GST_VIDEO_COLOR_MATRIX_BT601,
                   "unsupported YUV matrix (only SDR BT.601/709 supported)");
            ensure(color.range == GST_VIDEO_COLOR_RANGE_0_255 || color.range == GST_VIDEO_COLOR_RANGE_16_235, "unknown YUV range");
            attrs.insert(attrs.end(), {EGL_YUV_COLOR_SPACE_HINT_EXT,color.matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? EGL_ITU_REC709_EXT : EGL_ITU_REC601_EXT,
                EGL_SAMPLE_RANGE_HINT_EXT,color.range == GST_VIDEO_COLOR_RANGE_0_255 ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT});
        }
        attrs.push_back(EGL_NONE);
    }
};
}

struct EGLVideoInterop::Impl {
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
    GLuint program = 0, vbo = 0, fbo = 0, output = 0;
    SDL_Texture* texture = nullptr;
    int width = 0, height = 0;
    struct Pending { GstSample* sample; EGLImageKHR image; GLuint texture; EGLSyncKHR fence; };
    std::vector<Pending> pending;
    std::map<std::pair<guint32,guint64>, bool> supported;
    explicit Impl(SDL_Renderer* r) : renderer(r) {}
    void retire(bool drain) {
        for (auto it = pending.begin(); it != pending.end();) {
            const EGLint result = waitSync(display, it->fence, drain ? EGL_SYNC_FLUSH_COMMANDS_BIT_KHR : 0,
                                           drain ? EGL_FOREVER_KHR : 0);
            if (result == EGL_TIMEOUT_EXPIRED_KHR) { ++it; continue; }
            if (result != EGL_CONDITION_SATISFIED_KHR) glFinish(); // failure recovery only
            destroySync(display, it->fence);
            glDeleteTextures(1, &it->texture); destroyImage(display, it->image); gst_sample_unref(it->sample);
            it = pending.erase(it);
        }
    }
    bool supports(const Frame& frame) {
        auto key = std::make_pair(frame.drm.drm_fourcc, frame.drm.drm_modifier);
        if (auto found = supported.find(key); found != supported.end()) return found->second;
        EGLint count = 0;
        bool ok = queryModifiers(display, key.first, 0, nullptr, nullptr, &count);
        std::vector<EGLuint64KHR> mods(ok ? count : 0);
        std::vector<EGLBoolean> external(mods.size());
        if (ok && count) ok = queryModifiers(display, key.first, count, mods.data(), external.data(), &count);
        bool match = false;
        if (ok) for (EGLint i=0; i<count; ++i) if (mods[i] == key.second) match = true;
        // Bound capability cache for unusual streams; no FD/import caching.
        if (supported.size() >= 128) supported.clear();
        supported.emplace(key, match); return match;
    }
    void resize(int w, int h) {
        if (texture && width == w && height == h) return;
        // Retire old conversion inputs before changing allocation. SDL flush
        // already submitted all previous reads of the old destination.
        retire(true);
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
        if (output) glDeleteTextures(1, &output);
        output = 0;
        glGenTextures(1,&output); glBindTexture(GL_TEXTURE_2D,output);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        auto props = SDL_CreateProperties();
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,w);
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,h);
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER,output);
        texture = SDL_CreateTextureWithProperties(renderer,props); SDL_DestroyProperties(props);
        ensure(texture,"SDL EGL output wrapper creation failed"); width=w; height=h;
    }
    ~Impl() {
        if (!native) return;
        try {
            Context current(renderer,native); SDL_FlushRenderer(renderer);
            if (!pending.empty()) retire(true);
            if (texture) SDL_DestroyTexture(texture);
            if (output) glDeleteTextures(1,&output);
            if (fbo) glDeleteFramebuffers(1,&fbo);
            if (vbo) glDeleteBuffers(1,&vbo);
            if (program) glDeleteProgram(program);
            SDL_FlushRenderer(renderer);
        } catch (...) { LOG_ERROR("GStreamerVideo","EGL cleanup could not activate its renderer context"); }
    }
};

EGLVideoInterop::EGLVideoInterop(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(renderer)) {
    auto& p=*impl_;
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
        const GLfloat quad[]={-1,-1,1,-1,-1,1,1,1};
        GLint oldBuffer=0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&oldBuffer);
        glGenBuffers(1,&p.vbo); glBindBuffer(GL_ARRAY_BUFFER,p.vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER,oldBuffer); glGenFramebuffers(1,&p.fbo);
        p.ready=true; SDL_FlushRenderer(renderer);
    } catch (const std::exception& e) { p.error=e.what(); }
}
EGLVideoInterop::~EGLVideoInterop()=default;
bool EGLVideoInterop::available() const { return impl_->ready; }
const char* EGLVideoInterop::reason() const { return impl_->error.c_str(); }
int EGLVideoInterop::width() const { return impl_->width; }
int EGLVideoInterop::height() const { return impl_->height; }
GstElement* EGLVideoInterop::wrapSink(GstElement* sink) {
    auto* pad=gst_element_get_static_pad(sink,"sink");
    // A query probe handles allocation independently of appsink callback ABI.
    gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,[](GstPad*,GstPadProbeInfo* info,gpointer) {
        auto* query=GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(query)!=GST_QUERY_ALLOCATION) return GST_PAD_PROBE_OK;
        if (!gst_query_find_allocation_meta(query,GST_VIDEO_META_API_TYPE,nullptr))
            gst_query_add_allocation_meta(query,GST_VIDEO_META_API_TYPE,nullptr);
        return GST_PAD_PROBE_HANDLED;
    },nullptr,nullptr);
    gst_object_unref(pad); return sink;
}
void EGLVideoInterop::discardFrames() {
    auto& p=*impl_; if (!p.ready) return;
    Context current(p.renderer,p.native); SDL_FlushRenderer(p.renderer); p.retire(true);
    // Keep output, wrapper, FBO and shader for compatible future media.
}
SDL_Texture* EGLVideoInterop::copy(GstSample* sample) {
    auto& p=*impl_; if (!p.ready) return nullptr;
    EGLImageKHR image=EGL_NO_IMAGE_KHR; GLuint input=0;
    try {
        Frame frame(sample);
        Context current(p.renderer,p.native);
        ensure(p.supports(frame),"EGL does not advertise this DRM format/modifier");
        SDL_FlushRenderer(p.renderer); p.retire(p.pending.size()>=4);
        GLint framebuffer=0,viewport[4]{},enabled=0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer); glGetIntegerv(GL_VIEWPORT,viewport);
        glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&enabled);
        struct Restore {
            SDL_Renderer* r; GLint fb; GLint* viewport; GLint enabled;
            ~Restore() {
                if (enabled) glEnableVertexAttribArray(0); else glDisableVertexAttribArray(0);
                glBindBuffer(GL_ARRAY_BUFFER,0); glBindFramebuffer(GL_FRAMEBUFFER,fb);
                glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES,0); glUseProgram(0);
                SDL_FlushRenderer(r);
            }
        } restore{p.renderer,framebuffer,viewport,enabled};
        p.resize(frame.crop.w,frame.crop.h);
        image=p.createImage(p.display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,frame.attrs.data());
        ensure(image!=EGL_NO_IMAGE_KHR,"EGL DMA-BUF image import failed");
        glActiveTexture(GL_TEXTURE0); glGenTextures(1,&input); glBindTexture(GL_TEXTURE_EXTERNAL_OES,input);
        p.imageTarget(GL_TEXTURE_EXTERNAL_OES,image);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER,p.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,p.output,0);
        ensure(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"EGL conversion FBO incomplete");
        glViewport(0,0,p.width,p.height); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glUseProgram(p.program); glUniform1i(glGetUniformLocation(p.program,"video"),0);
        auto* meta=gst_buffer_get_video_meta(gst_sample_get_buffer(sample));
        glUniform4f(glGetUniformLocation(p.program,"crop"),float(frame.crop.x)/meta->width,float(frame.crop.y)/meta->height,
                    float(frame.crop.w)/meta->width,float(frame.crop.h)/meta->height);
        glBindBuffer(GL_ARRAY_BUFFER,p.vbo); glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4); ensure(glGetError()==GL_NO_ERROR,"EGL external conversion GL error");
        auto fence=p.createSync(p.display,EGL_SYNC_FENCE_KHR,nullptr);
        ensure(fence!=EGL_NO_SYNC_KHR,"EGL conversion fence creation failed");
        p.pending.push_back({gst_sample_ref(sample),image,input,fence}); image=EGL_NO_IMAGE_KHR; input=0;
        glFlush(); return p.texture;
    } catch (const std::exception& e) {
        p.error=e.what();
        // Preserve the exact negotiated metadata: an unknown matrix and an
        // explicitly unsupported matrix must be distinguishable in reports.
        if (auto* caps = gst_sample_get_caps(sample)) {
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
        try { Context current(p.renderer,p.native); glFinish(); if(input)glDeleteTextures(1,&input); if(image!=EGL_NO_IMAGE_KHR)p.destroyImage(p.display,image); } catch (...) {}
        return nullptr;
    }
}
