// Standalone EGL/VA-API experiment. No RetroFE objects or lifecycle code.
#include <SDL3/SDL.h>
#include <gst/app/gstappsink.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/video/video.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdlib>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
}

int inspectDmaBuf(const char* file);

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: gst_sdl3_linux FILE.mp4 [nv12|rgba|dmabuf-inspect]\n"
                     "Space: unload/reopen same pipeline; R: rebuild pipeline; Escape: exit\n";
        return 2;
    }
    if (argc > 2 && std::string(argv[2]) == "dmabuf-inspect") {
        gst_init(nullptr, nullptr);
        return inspectDmaBuf(argv[1]);
    }
    const bool nv12 = argc < 3 || std::string(argv[2]) == "nv12";
    if (argc > 2 && std::string(argv[2]) != "nv12" && std::string(argv[2]) != "rgba") return 2;
    gst_init(&argc, &argv);
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    GstGLDisplay* display = nullptr;
    GstGLContext* context = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* sink = nullptr;
    GstBus* bus = nullptr;
    GstSample* held = nullptr;
    SDL_Texture* texture = nullptr;
    GstContext* displayContext = nullptr;
    GstContext* appContext = nullptr;
    int result = 0;
    // Blocking completion is intentional: establish a correctness baseline
    // before introducing asynchronous retirement or wrapper caching.
    auto releaseFrame = [&]() {
        if (renderer) SDL_FlushRenderer(renderer);
        if (context && context->gl_vtable->Finish) context->gl_vtable->Finish();
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
        if (held) gst_sample_unref(held);
        held = nullptr;
    };
    auto destroyPipeline = [&]() {
        releaseFrame();
        if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) gst_object_unref(bus);
        if (sink) gst_object_unref(sink);
        if (pipeline) gst_object_unref(pipeline);
        bus = nullptr; sink = nullptr; pipeline = nullptr;
    };
    try {
        auto envNumber = [](const char* name) -> Uint64 {
            const char* value = std::getenv(name);
            if (!value) return 0;
            std::string text(value);
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error(std::string("Invalid nonnegative integer: ") + name);
            return std::stoull(text);
        };
        const Uint64 cycleMs = envNumber("PROTO_CYCLE_MS");
        const Uint64 rebuildEvery = envNumber("PROTO_REBUILD_EVERY");
        const Uint64 durationMs = envNumber("PROTO_DURATION_MS");
        require(SDL_Init(SDL_INIT_VIDEO), "SDL init");
        gchar* gstVersion = gst_version_string();
        std::cout << gstVersion << "; SDL runtime " << SDL_GetVersion()
                  << "; cycle-ms=" << cycleMs << "; rebuild-every=" << rebuildEvery << std::endl;
        g_free(gstVersion);
        window = SDL_CreateWindow("GStreamer / SDL3 plane prototype", 960, 540, 0);
        require(window != nullptr, "window");
        renderer = SDL_CreateRenderer(window, "opengles2");
        require(renderer != nullptr, "EGL renderer");
        auto native = SDL_GL_GetCurrentContext();
        require(native && SDL_GL_MakeCurrent(window, native), "current context");
        auto egl = SDL_EGL_GetCurrentDisplay();
        require(egl != nullptr, "EGL display required");
        display = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(egl));
        auto api = gst_gl_context_get_current_gl_api(GST_GL_PLATFORM_EGL, nullptr, nullptr);
        gst_gl_display_filter_gl_api(display, api);
        context = gst_gl_context_new_wrapped(display,
            gst_gl_context_get_current_gl_context(GST_GL_PLATFORM_EGL), GST_GL_PLATFORM_EGL, api);
        require(context && gst_gl_context_activate(context, TRUE), "wrap context");
        GError* error = nullptr;
        if (!gst_gl_context_fill_info(context, &error)) {
            std::string message = error ? error->message : "GL info failed";
            g_clear_error(&error); throw std::runtime_error(message);
        }
        displayContext = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(displayContext, display);
        appContext = gst_context_new("gst.gl.app_context", TRUE);
        gst_structure_set(gst_context_writable_structure(appContext), "context", GST_TYPE_GL_CONTEXT, context, nullptr);
        auto createPipeline = [&]() {
            const std::string chain = std::string("filesrc name=input ! qtdemux ! h264parse ! vah264dec ! ") +
                "video/x-raw(memory:DMABuf),format=DMA_DRM ! glupload ! " +
                (nv12 ? "" : "glcolorconvert ! ") +
                "video/x-raw(memory:GLMemory),format=" + (nv12 ? "NV12" : "RGBA") +
                ",texture-target=2D ! appsink name=output max-buffers=1 drop=true sync=true enable-last-sample=false";
            pipeline = gst_parse_launch(chain.c_str(), &error);
            if (error) { std::string message = error->message; g_clear_error(&error); throw std::runtime_error(message); }
            require(pipeline != nullptr, "pipeline");
            auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
            g_object_set(input, "location", argv[1], nullptr);
            gst_object_unref(input);
            gst_element_set_context(pipeline, displayContext);
            gst_element_set_context(pipeline, appContext);
            sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
            bus = gst_element_get_bus(pipeline);
            require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "start");
            std::cout << chain << std::endl;
        };
        createPipeline();
        bool running = true;
        unsigned frames = 0;
        bool logFrame = true;
        Uint64 cycles = 0;
        const Uint64 started = SDL_GetTicks();
        Uint64 lastCycle = started;
        auto cycle = [&](bool rebuild) {
            if (rebuild) { destroyPipeline(); createPipeline(); }
            else {
                releaseFrame();
                require(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "unload");
                require(gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS, "unload completion");
                require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "reopen");
            }
            ++cycles;
            logFrame = true;
            lastCycle = SDL_GetTicks();
            std::cout << "Cycle " << cycles << (rebuild ? " rebuilt" : " reopened")
                      << "; total frames=" << frames << std::endl;
        };
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) running = false;
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (event.key.key == SDLK_SPACE) cycle(false);
                    else if (event.key.key == SDLK_R) cycle(true);
                }
            }
            if (!running || (durationMs && SDL_GetTicks() - started >= durationMs)) break;
            if (cycleMs && SDL_GetTicks() - lastCycle >= cycleMs)
                cycle(rebuildEvery && (cycles + 1) % rebuildEvery == 0);
            while (auto* message = gst_bus_pop(bus)) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    gchar* debug = nullptr;
                    gst_message_parse_error(message, &error, &debug);
                    std::string detail = error->message;
                    if (debug) detail += std::string("\n") + debug;
                    g_clear_error(&error); g_free(debug); gst_message_unref(message);
                    throw std::runtime_error(detail);
                }
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                    require(gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0), "loop");
                }
                gst_message_unref(message);
            }
            if (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) {
                releaseFrame();
                held = sample;
                auto* buffer = gst_sample_get_buffer(sample);
                GstVideoInfo info{};
                require(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)), "video info");
                if (logFrame) {
                    gchar* capsText = gst_caps_to_string(gst_sample_get_caps(sample));
                    std::cout << "Negotiated output: " << capsText << std::endl;
                    g_free(capsText);
                }
                const unsigned planes = nv12 ? 2 : 1;
                require(GST_VIDEO_INFO_FORMAT(&info) == (nv12 ? GST_VIDEO_FORMAT_NV12 : GST_VIDEO_FORMAT_RGBA), "format");
                require(gst_buffer_n_memory(buffer) == planes, "one GL memory per plane required");
                SDL_FlushRenderer(renderer);
                for (unsigned i = 0; i < planes; ++i) {
                    auto* memory = gst_buffer_peek_memory(buffer, i);
                    require(gst_is_gl_memory(memory), "GL memory required");
                    auto* glmem = reinterpret_cast<GstGLMemory*>(memory);
                    require(gst_gl_memory_get_texture_target(glmem) == GST_GL_TEXTURE_TARGET_2D, "2D planes required");
                    require(gst_gl_context_can_share(context, glmem->mem.context), "context sharing");
                    // Explicit synchronous producer completion for this experiment.
                    gst_gl_context_thread_add(glmem->mem.context, [](GstGLContext* c, gpointer) { c->gl_vtable->Finish(); }, nullptr);
                    if (logFrame) std::cout << "Plane " << i << " GL format=" << glmem->tex_format
                        << " allocated=" << gst_gl_memory_get_texture_width(glmem) << "x" << gst_gl_memory_get_texture_height(glmem)
                        << " stride=" << info.stride[i] << std::endl;
                    // Start with unpadded planes; never guess UV scaling.
                    const int w = i ? (info.width + 1) / 2 : info.width;
                    const int h = i ? (info.height + 1) / 2 : info.height;
                    require(gst_gl_memory_get_texture_width(glmem) == w && gst_gl_memory_get_texture_height(glmem) == h, "padded planes need explicit layout handling");
                }
                GstVideoFrame frame{};
                require(gst_video_frame_map(&frame, &info, buffer, static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_GL)), "GL frame map");
                auto props = SDL_CreateProperties();
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_ABGR8888);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, info.width);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, info.height);
                const auto color = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709
                    ? (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED)
                    : (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, nv12 ? color : SDL_COLORSPACE_SRGB);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_NUMBER,
                    *static_cast<GLuint*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)));
                if (nv12) SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_OPENGLES2_TEXTURE_UV_NUMBER,
                    *static_cast<GLuint*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1)));
                texture = SDL_CreateTextureWithProperties(renderer, props);
                SDL_DestroyProperties(props);
                gst_video_frame_unmap(&frame);
                require(texture != nullptr, "wrap planes");
                logFrame = false;
                if (++frames == 1 || frames % 300 == 0) std::cout << "Wrapped " << frames << " " << (nv12 ? "NV12" : "RGBA") << " frames; inspect colors and orientation" << std::endl;
            }
            SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
            SDL_RenderClear(renderer);
            if (texture) require(SDL_RenderTexture(renderer, texture, nullptr, nullptr), "draw");
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
            SDL_FRect overlay{20, 20, 80, 40};
            SDL_RenderFillRect(renderer, &overlay);
            require(SDL_RenderPresent(renderer), "present");
            SDL_Delay(8);
        }
        std::cout << "Finished: " << frames << " frames, " << cycles << " cycles, "
                  << SDL_GetTicks() - started << " ms" << std::endl;
        require(frames != 0, "no frames rendered");
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; result = 1; }
    destroyPipeline();
    if (appContext) gst_context_unref(appContext);
    if (displayContext) gst_context_unref(displayContext);
    if (context) { gst_gl_context_activate(context, FALSE); gst_object_unref(context); }
    if (display) gst_object_unref(display);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
