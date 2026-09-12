// DMA-BUF -> external EGL texture -> persistent RGBA FBO -> SDL.
// All GL operations occur on SDL's thread/context. No GStreamer GL objects.
#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/video/video-info-dma.h>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
GLuint shader(GLenum type, const char* source) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);
    GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]{}; glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        glDeleteShader(id); throw std::runtime_error(log);
    }
    return id;
}
Uint64 option(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return 0;
    const std::string text(value);
    check(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, "invalid stress option");
    return std::stoull(text);
}
}

void playExternal(SDL_Renderer* renderer, GstElement* pipeline, GstElement* sink,
                  GstBus* bus, GstSample* first) {
    auto display = static_cast<EGLDisplay>(SDL_EGL_GetCurrentDisplay());
    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto imageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    check(createImage && destroyImage && imageTarget, "EGL image entry points unavailable");
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    check(extensions && (std::string(" ") + extensions + " ").find(" GL_OES_EGL_image_external ") != std::string::npos,
          "GL_OES_EGL_image_external unavailable");
    GLuint program = 0, vertex = 0, fragment = 0, vbo = 0, output = 0, fbo = 0, input = 0;
    SDL_Texture* texture = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GstSample* held = nullptr;
    int width = 0, height = 0;
    auto releaseInput = [&]() {
        // Finish conversion before VA may recycle the imported surface.
        glFinish();
        if (input) glDeleteTextures(1, &input);
        input = 0;
        if (image != EGL_NO_IMAGE_KHR) destroyImage(display, image);
        image = EGL_NO_IMAGE_KHR;
        if (held) gst_sample_unref(held);
        held = nullptr;
    };
    auto cleanup = [&]() {
        SDL_FlushRenderer(renderer); glFinish(); releaseInput();
        if (texture) SDL_DestroyTexture(texture);
        if (output) glDeleteTextures(1, &output);
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (program) glDeleteProgram(program);
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_FlushRenderer(renderer);
    };
    try {
        SDL_FlushRenderer(renderer);
        vertex = shader(GL_VERTEX_SHADER, "attribute vec2 pos; varying vec2 uv; void main(){uv=(pos+1.0)*0.5;gl_Position=vec4(pos,0.0,1.0);}");
        fragment = shader(GL_FRAGMENT_SHADER, "#extension GL_OES_EGL_image_external : require\nprecision mediump float; varying vec2 uv; uniform samplerExternalOES video; void main(){gl_FragColor=texture2D(video,uv);}");
        program = glCreateProgram(); glAttachShader(program, vertex); glAttachShader(program, fragment);
        glBindAttribLocation(program, 0, "pos"); glLinkProgram(program);
        GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[4096]{}; glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            throw std::runtime_error(log);
        }
        const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
        glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glGenFramebuffers(1, &fbo);
        const Uint64 cycleMs = option("PROTO_CYCLE_MS"), duration = option("PROTO_DURATION_MS");
        const Uint64 rebuildEvery = option("PROTO_REBUILD_EVERY");
        const Uint64 start = SDL_GetTicks(); Uint64 lastCycle = start, frames = 0, cycles = 0;
        bool running = true, initial = true, visible = false;
        auto cycle = [&](bool reset) {
            SDL_FlushRenderer(renderer); releaseInput(); visible = false;
            check(gst_element_set_state(pipeline, reset ? GST_STATE_NULL : GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "state transition failed");
            check(gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS, "state transition timeout");
            check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "restart failed");
            ++cycles; lastCycle = SDL_GetTicks(); initial = false;
            std::cout << "EGL cycle " << cycles << (reset ? " NULL reset" : " READY reopen") << std::endl;
        };
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) running = false;
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (event.key.key == SDLK_SPACE) cycle(false);
                    if (event.key.key == SDLK_R) cycle(true);
                }
            }
            if (!running || (duration && SDL_GetTicks() - start >= duration)) break;
            if (cycleMs && SDL_GetTicks() - lastCycle >= cycleMs) cycle(rebuildEvery && (cycles + 1) % rebuildEvery == 0);
            while (auto* message = gst_bus_pop(bus)) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError* error = nullptr; gchar* debug = nullptr;
                    gst_message_parse_error(message, &error, &debug);
                    std::string why(error->message); if (debug) why += std::string("\n") + debug;
                    g_clear_error(&error); g_free(debug); gst_message_unref(message); throw std::runtime_error(why);
                }
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS)
                    check(gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0), "loop failed");
                gst_message_unref(message);
            }
            held = initial ? gst_sample_ref(first) : gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0);
            initial = false;
            if (held) {
                SDL_FlushRenderer(renderer);
                GLint previousFramebuffer = 0, previousViewport[4]{}, positionEnabled = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
                glGetIntegerv(GL_VIEWPORT, previousViewport);
                glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &positionEnabled);
                GstVideoInfoDmaDrm drm{}; gst_video_info_dma_drm_init(&drm);
                check(gst_video_info_dma_drm_from_caps(&drm, gst_sample_get_caps(held)), "DMA caps failed");
                auto* buffer = gst_sample_get_buffer(held);
                auto* meta = gst_buffer_get_video_meta(buffer);
                check(meta && meta->n_planes == 2, "prototype requires two-plane NV12 metadata");
                check(drm.drm_fourcc == 0x3231564e, "prototype requires NV12 FourCC");
                check(meta->width == static_cast<guint>(drm.vinfo.width) && meta->height == static_cast<guint>(drm.vinfo.height), "cropped allocation not supported yet");
                check(!gst_buffer_get_video_crop_meta(buffer), "explicit crop metadata not supported yet");
                std::vector<EGLint> attrs{EGL_WIDTH, drm.vinfo.width, EGL_HEIGHT, drm.vinfo.height,
                    EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(drm.drm_fourcc)};
                const EGLint fdKeys[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT};
                const EGLint offsetKeys[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT};
                const EGLint pitchKeys[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT};
                const EGLint lowKeys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT};
                const EGLint highKeys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT};
                for (guint i = 0; i < 2; ++i) {
                    guint index = 0, count = 0; gsize skip = 0, offset = 0;
                    check(gst_buffer_find_memory(buffer, meta->offset[i], 1, &index, &count, &skip), "plane offset invalid");
                    auto* memory = gst_buffer_peek_memory(buffer, index);
                    check(gst_is_dmabuf_memory(memory), "plane is not DMA-BUF");
                    gst_memory_get_sizes(memory, &offset, nullptr);
                    check(offset <= INT_MAX && skip <= INT_MAX - offset && meta->stride[i] > 0, "invalid EGL pitch/offset");
                    attrs.insert(attrs.end(), {fdKeys[i], gst_dmabuf_memory_get_fd(memory), offsetKeys[i], static_cast<EGLint>(offset + skip),
                        pitchKeys[i], meta->stride[i], lowKeys[i], static_cast<EGLint>(drm.drm_modifier & 0xffffffffu),
                        highKeys[i], static_cast<EGLint>(drm.drm_modifier >> 32)});
                }
                const auto color = drm.vinfo.colorimetry;
                check(color.matrix == GST_VIDEO_COLOR_MATRIX_BT709 || color.matrix == GST_VIDEO_COLOR_MATRIX_BT601, "only BT.601/709 supported");
                check(color.range == GST_VIDEO_COLOR_RANGE_0_255 || color.range == GST_VIDEO_COLOR_RANGE_16_235, "unknown YUV range");
                attrs.insert(attrs.end(), {EGL_YUV_COLOR_SPACE_HINT_EXT, color.matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? EGL_ITU_REC709_EXT : EGL_ITU_REC601_EXT,
                    EGL_SAMPLE_RANGE_HINT_EXT, color.range == GST_VIDEO_COLOR_RANGE_0_255 ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT, EGL_NONE});
                image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
                if (image == EGL_NO_IMAGE_KHR) throw std::runtime_error("eglCreateImageKHR failed, EGL error=" + std::to_string(eglGetError()));
                glActiveTexture(GL_TEXTURE0); glGenTextures(1, &input); glBindTexture(GL_TEXTURE_EXTERNAL_OES, input);
                imageTarget(GL_TEXTURE_EXTERNAL_OES, image);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                if (width != drm.vinfo.width || height != drm.vinfo.height) {
                    glFinish();
                    if (texture) SDL_DestroyTexture(texture);
                    texture = nullptr;
                    if (output) glDeleteTextures(1, &output);
                    width = drm.vinfo.width; height = drm.vinfo.height;
                    glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    auto props = SDL_CreateProperties();
                    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
                    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
                    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
                    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER, output);
                    texture = SDL_CreateTextureWithProperties(renderer, props); SDL_DestroyProperties(props);
                    check(texture, "SDL RGBA wrapper failed");
                    SDL_FlushRenderer(renderer);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
                check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA framebuffer incomplete");
                glViewport(0, 0, width, height); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glUseProgram(program); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, input);
                glUniform1i(glGetUniformLocation(program, "video"), 0);
                glBindBuffer(GL_ARRAY_BUFFER, vbo); glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                check(glGetError() == GL_NO_ERROR, "external texture conversion GL error");
                // SDL GLES keeps its position array enabled across draws.
                // Disabling it unconditionally collapses subsequent SDL geometry.
                if (positionEnabled) glEnableVertexAttribArray(0);
                else glDisableVertexAttribArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
                glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0); glUseProgram(0);
                releaseInput();
                SDL_FlushRenderer(renderer); // invalidate SDL state after custom GL rendering
                visible = true;
                if (++frames == 1 || frames % 300 == 0) std::cout << "EGL imported/converted " << frames << " frames; persistent RGBA output, no GStreamer GL context" << std::endl;
            }
            SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255); SDL_RenderClear(renderer);
            if (visible) check(SDL_RenderTexture(renderer, texture, nullptr, nullptr), "SDL draw failed");
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); SDL_FRect rect{20,20,80,40}; SDL_RenderFillRect(renderer, &rect);
            check(SDL_RenderPresent(renderer), "SDL present failed"); SDL_Delay(8);
        }
        std::cout << "EGL mode finished: " << frames << " frames, " << cycles << " cycles\n";
    } catch (...) { cleanup(); throw; }
    cleanup();
}
