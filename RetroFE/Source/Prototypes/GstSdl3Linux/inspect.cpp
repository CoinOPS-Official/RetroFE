#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/video/video-info-dma.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Inspection only: no GstGLContext, GL texture import, or CPU pixel mapping.
int inspectDmaBuf(const char* file) {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* sink = nullptr;
    GstSample* sample = nullptr;
    GstBus* bus = nullptr;
    int result = 0;
    auto check = [](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); };
    try {
        check(SDL_Init(SDL_INIT_VIDEO), "SDL video init failed");
        window = SDL_CreateWindow("DMA-BUF capability inspection", 640, 360, 0);
        check(window, "SDL window failed");
        renderer = SDL_CreateRenderer(window, "opengles2");
        check(renderer, "SDL GLES renderer failed");
        EGLDisplay display = static_cast<EGLDisplay>(SDL_EGL_GetCurrentDisplay());
        check(display != EGL_NO_DISPLAY, "SDL renderer has no current EGL display");
        const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
        std::cout << "SDL=" << SDL_GetVersion() << " EGL extensions: "
                  << (extensions ? extensions : "unavailable") << std::endl;
        GError* error = nullptr;
        pipeline = gst_parse_launch("filesrc name=input ! qtdemux ! h264parse ! vah264dec ! "
            "video/x-raw(memory:DMABuf),format=DMA_DRM ! appsink name=output "
            "max-buffers=1 drop=true sync=false enable-last-sample=false", &error);
        if (error) { std::string why(error->message); g_clear_error(&error); throw std::runtime_error(why); }
        check(pipeline, "pipeline creation failed");
        auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        g_object_set(input, "location", file, nullptr);
        gst_object_unref(input);
        sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
        // DMA-BUF export requires a consumer that understands non-default
        // plane layouts. Caps alone do not advertise this allocation contract.
        g_object_set(sink, "emit-signals", TRUE, nullptr);
        g_signal_connect(sink, "propose-allocation", G_CALLBACK(+[](GstElement*, GstQuery* query, gpointer) -> gboolean {
            if (!gst_query_find_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr))
                gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
            std::cout << "Allocation: advertising GstVideoMeta support; decoder supplies DMA-BUF pool" << std::endl;
            return TRUE;
        }), nullptr);
        bus = gst_element_get_bus(pipeline);
        check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "start failed");
        const Uint64 start = SDL_GetTicks();
        while (!sample && SDL_GetTicks() - start < 15000) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) throw std::runtime_error("inspection cancelled");
            }
            sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
            if (auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                std::string why(error->message);
                if (debug) why += std::string("\n") + debug;
                g_clear_error(&error); g_free(debug); gst_message_unref(message);
                throw std::runtime_error(why);
            }
            if (!sample && gst_app_sink_is_eos(GST_APP_SINK(sink))) break;
        }
        check(sample, "no DMA-BUF sample within 15 seconds");
        auto* caps = gst_sample_get_caps(sample);
        gchar* capsText = gst_caps_to_string(caps);
        std::cout << "Caps: " << capsText << std::endl;
        g_free(capsText);
        GstVideoInfoDmaDrm drm{};
        gst_video_info_dma_drm_init(&drm);
        check(gst_video_info_dma_drm_from_caps(&drm, caps), "cannot parse DMA_DRM caps");
        std::cout << "FourCC=0x" << std::hex << drm.drm_fourcc << " modifier=0x"
                  << drm.drm_modifier << std::dec << std::endl;
        auto* buffer = gst_sample_get_buffer(sample);
        auto* meta = gst_buffer_get_video_meta(buffer);
        check(meta, "GstVideoMeta absent; refusing to guess hardware plane layout");
        std::cout << "VideoMeta " << meta->width << "x" << meta->height
                  << " planes=" << meta->n_planes << " memory objects=" << gst_buffer_n_memory(buffer) << std::endl;
        for (guint i = 0; i < gst_buffer_n_memory(buffer); ++i) {
            auto* memory = gst_buffer_peek_memory(buffer, i);
            check(gst_is_dmabuf_memory(memory), "non-DMA-BUF memory in sample");
            gsize offset = 0, maxsize = 0;
            const gsize size = gst_memory_get_sizes(memory, &offset, &maxsize);
            std::cout << "Memory " << i << " borrowed fd=" << gst_dmabuf_memory_get_fd(memory)
                      << " offset=" << offset << " size=" << size << " maxsize=" << maxsize << std::endl;
        }
        for (guint i = 0; i < meta->n_planes; ++i) {
            guint index = 0, length = 0;
            gsize skip = 0;
            check(gst_buffer_find_memory(buffer, meta->offset[i], 1, &index, &length, &skip), "plane start outside buffer");
            auto* memory = gst_buffer_peek_memory(buffer, index);
            gsize memoryOffset = 0;
            gst_memory_get_sizes(memory, &memoryOffset, nullptr);
            std::cout << "Plane " << i << " stride=" << meta->stride[i]
                      << " buffer-offset=" << meta->offset[i] << " memory-index=" << index
                      << " fd=" << gst_dmabuf_memory_get_fd(memory)
                      << " fd-offset=" << memoryOffset + skip << std::endl;
        }
        const std::string ext = std::string(" ") + (extensions ? extensions : "") + " ";
        check(ext.find(" EGL_EXT_image_dma_buf_import_modifiers ") != std::string::npos,
              "EGL modifier queries unavailable; support remains unknown");
        auto query = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        check(query, "EGL modifier query entry point unavailable");
        EGLint count = 0;
        check(query(display, static_cast<EGLint>(drm.drm_fourcc), 0, nullptr, nullptr, &count), "EGL modifier count query failed");
        std::vector<EGLuint64KHR> modifiers(count);
        std::vector<EGLBoolean> external(count);
        if (count) check(query(display, static_cast<EGLint>(drm.drm_fourcc), count,
            modifiers.data(), external.data(), &count), "EGL modifier query failed");
        bool found = false;
        for (EGLint i = 0; i < count; ++i) {
            if (modifiers[i] != drm.drm_modifier) continue;
            found = true;
            std::cout << "Exact format/modifier advertised by EGL; external_only="
                      << (external[i] ? "true" : "false") << std::endl;
        }
        if (!found) std::cout << "Exact modifier NOT advertised by EGL; no compatible import established\n";
        std::cout << "Inspection complete. No image imported or rendered; advertised support is not proof of SDL plane compatibility.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (SDL: " << SDL_GetError() << ")\n"; result = 1;
    }
    if (sample) gst_sample_unref(sample);
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (sink) gst_object_unref(sink);
    if (pipeline) gst_object_unref(pipeline);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
