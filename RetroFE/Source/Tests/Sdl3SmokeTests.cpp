#include "../SDL.h"
#include "../Database/Configuration.h"
#include "../Control/UserInput.h"
#include "../Sound/AudioBus.h"
#include "../Sound/Sound.h"
#include "../Graphics/Font.h"
#include "../Graphics/GeometryBatch.h"
#include "../Graphics/Component/Text.h"
#include "../Graphics/FontCache.h"
#include "../Video/GStreamerVideo.h"
#include "../Video/GlibLoop.h"
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>

std::vector<std::string> settingsFromCLI;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << ": " << SDL_GetError() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

SDL_Color pixel(SDL_Renderer* renderer, int x, int y) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    require(surface != nullptr, "Read rendered pixels");
    SDL_Color result{};
    require(SDL_ReadSurfacePixel(surface, x, y, &result.r, &result.g, &result.b, &result.a), "Read pixel");
    SDL_DestroySurface(surface);
    return result;
}

void batchChecks(SDL_Renderer* renderer, SDL_Texture* texture) {
    auto* alternate = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 4, 4);
    require(alternate != nullptr, "Create alternate atlas");
    Uint32 pixels[16];
    std::fill_n(pixels, 16, 0xffffffffu);
    require(SDL_UpdateTexture(alternate, nullptr, pixels, 16), "Upload alternate atlas");
    require(SDL_SetTextureBlendMode(alternate, SDL_BLENDMODE_BLEND), "Set alternate blend mode");

    auto capture = [&]() {
        auto* image = SDL_RenderReadPixels(renderer, nullptr);
        require(image != nullptr, "Capture geometry result");
        auto* rgba = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(image);
        require(rgba != nullptr, "Normalize geometry capture");
        return rgba;
    };
    for (bool offscreen : {false, true}) {
        SDL_Surface* reference = nullptr;
        for (bool batched : {false, true}) {
            require(SDL_RenderClear(renderer), "Clear batch comparison");
            GeometryBatch batch;
            // Exceed capacity, alternate A/B/A, overlap translucent quads, and
            // change tint while a run is pending. Mirror/output rotation are
            // also exercised by the caller's later renderChecks invocation.
            for (int i = 0; i < 320; ++i) {
                auto* atlas = (i == 280 || i == 285) ? alternate : texture;
                require(SDL_SetTextureColorMod(atlas, Uint8(30 + i % 220), 80, 140), "Set run tint");
                SDL_Rect src{0, 0, 4, 4};
                SDL_FRect dst{float(4 + i % 25), float(4 + i % 21), 12, 10};
                if (offscreen) {
                    require(SDL_SetTextureAlphaMod(atlas, 110), "Set offscreen alpha");
                    if (batched) require(batch.appendTexture(renderer, atlas, src, dst), "Append atlas quad");
                    else {
                        SDL_FRect source{0, 0, 4, 4};
                        require(SDL_RenderTexture(renderer, atlas, &source, &dst), "Draw immediate atlas quad");
                    }
                } else {
                    ViewInfo view;
                    view.Angle = (i % 3 == 0) ? 27.0f : 0.0f;
                    view.ContainerX = view.ContainerY = 8;
                    view.ContainerWidth = view.ContainerHeight = 40;
                    view.hasReflection = i % 4 == 0;
                    view.reflectionMask = 15;
                    view.ReflectionScale = 0.6f;
                    view.ReflectionAlpha = 0.4f;
                    if (batched) require(SDL::appendCopyF(batch, atlas, 0.4f, &src, &dst, view, 64, 64), "Append transformed quad");
                    else require(SDL::renderCopyF(atlas, 0.4f, &src, &dst, view, 64, 64), "Draw immediate transformed quad");
                }
            }
            require(batch.flush(), "Flush geometry batch");
            auto* actual = capture();
            if (!batched) reference = actual;
            else {
                require(reference->w == actual->w && reference->h == actual->h, "Batch dimensions match");
                for (int y = 0; y < actual->h; ++y) {
                    require(std::memcmp(static_cast<Uint8*>(reference->pixels) + y * reference->pitch,
                        static_cast<Uint8*>(actual->pixels) + y * actual->pitch, actual->w * 4) == 0,
                        "Ordered batching preserves every pixel");
                }
                SDL_DestroySurface(reference);
                SDL_DestroySurface(actual);
            }
        }
    }
    SDL_DestroyTexture(alternate);
}

void renderChecks(Configuration& config, bool checkPixels) {
    require(SDL::initialize(config), "Initialize RetroFE SDL3 backend");
    auto* renderer = SDL::getRenderer(0);
    require(renderer != nullptr, "Create renderer");
    require(SDL_SetRenderTarget(renderer, SDL::getRenderTarget(0)), "Set target");
    require(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) && SDL_RenderClear(renderer), "Clear target");

    SDL_Surface* surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
    require(surface != nullptr, "Create image surface");
    require(SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGBA(surface, 255, 255, 255, 255)), "Fill image");
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    require(texture != nullptr, "Create texture");
    require(SDL_SetTextureColorMod(texture, 200, 100, 50), "Set texture tint");
    require(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND), "Set alpha blend");
    // Borderless output can be larger than the requested 64x64 layout.
    auto layoutPixel = [&](int x, int y) {
        auto* target = SDL_GetRenderTarget(renderer);
        return pixel(renderer, x * target->w / 64, y * target->h / 64);
    };
    ViewInfo view;
    SDL_FRect dest{8, 8, 16, 16};
    require(SDL::renderCopyF(texture, 0.5f, nullptr, &dest, view, 64, 64), "Draw tinted alpha geometry");
    if (checkPixels) {
        const auto color = layoutPixel(16, 16);
        require(color.r >= 95 && color.r <= 105 && color.g >= 45 && color.g <= 55 && color.b >= 20 && color.b <= 30,
            "SDL3 float vertex colors preserve tint and alpha");
        require(layoutPixel(2, 2).r == 0, "Geometry stays inside destination");
    }
    require(SDL_RenderClear(renderer), "Clear for reflection");
    view.hasReflection = true;
    view.reflectionMask = 2;
    view.ReflectionScale = 1.0f;
    view.ReflectionAlpha = 0.5f;
    require(SDL::renderCopyF(texture, 1.0f, nullptr, &dest, view, 64, 64), "Draw reflection");
    if (checkPixels) {
        require(layoutPixel(16, 16).r >= 195, "Primary image remains opaque");
        const auto reflected = layoutPixel(16, 32);
        require(reflected.r >= 95 && reflected.r <= 105, "Reflection has independent alpha");
    }
    require(SDL_RenderClear(renderer), "Clear for clipping");
    view.hasReflection = false;
    view.ContainerX = 16;
    view.ContainerY = 8;
    view.ContainerWidth = 8;
    view.ContainerHeight = 16;
    require(SDL::renderCopyF(texture, 1.0f, nullptr, &dest, view, 64, 64), "Draw clipped geometry");
    if (checkPixels) {
        require(layoutPixel(12, 16).r == 0, "Container clips left side");
        require(layoutPixel(20, 16).r >= 195, "Container preserves visible side");
    }
    batchChecks(renderer, texture);
    SDL_DestroyTexture(texture);
    require(SDL_SetRenderTarget(renderer, nullptr), "Restore backbuffer");
    require(SDL_RenderTexture(renderer, SDL::getRenderTarget(0), nullptr, nullptr), "Present target");
    require(SDL_RenderPresent(renderer), "Present frame");
}

void inputChecks(Configuration& config) {
    for (const auto* key : {"up", "down", "left", "right", "select", "back", "quit"})
        config.setProperty(std::string("controls.") + key, std::string("joy0button0"));
    UserInput input(config);
    require(input.initialize(), "Initialize input bindings");
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.nbuttons = 1;
    desc.name = "RetroFE SDL3 test joystick";
    const auto id = SDL_AttachVirtualJoystick(&desc);
    require(id != 0, "Attach virtual joystick");
    auto* joystick = SDL_OpenJoystick(id);
    require(joystick != nullptr, "Open virtual joystick");
    SDL_Event event;
    // Assign this virtual device first; physical controllers on the test host
    // must not change the configured slot under test.
    event = {};
    event.type = SDL_EVENT_JOYSTICK_ADDED;
    event.jdevice.which = id;
    input.update(event);
    while (SDL_PollEvent(&event)) {}
    require(SDL_SetJoystickVirtualButton(joystick, 0, true), "Press virtual button");
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&event)) {
        const auto original = event;
        if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.which == id)
            input.update(event);
        if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN)
            require(event.jbutton.which == original.jbutton.which, "Input mapping preserves event instance ID");
    }
    input.updateKeystate();
    require(input.keystate(UserInput::KeyCodeSelect), "Configured joy0 maps to SDL3 instance ID");
    SDL_CloseJoystick(joystick);
    require(SDL_DetachVirtualJoystick(id), "Detach virtual joystick");
    while (SDL_PollEvent(&event)) input.update(event);
    input.updateKeystate();
    require(!input.keystate(UserInput::KeyCodeSelect), "Disconnect clears held input");
}

void mediaChecks(const std::string& assets) {
    require(AudioBus::instance().mixer() != nullptr, "Open SDL3 audio mixer");
    Sound sound(assets + "/layouts/Arcades/sounds/select.wav", "");
    require(sound.allocate(), "Decode packaged WAV sound");
    sound.play();
    require(sound.isPlaying(), "Play sound through SDL3 track");
    sound.play();
    require(sound.isPlaying(), "Play overlapping sound voices");
    sound.free();
    require(!sound.isPlaying(), "Release sound tracks");
    require(TTF_Init(), "Initialize SDL3_ttf");
    {
        FontManager font(assets + "/retrofe/OpenSans.ttf", 24, {255,255,255,255}, true, 1, 0);
        require(font.initialize(), "Build gradient and outline font atlases");
        require(font.getWidth("RetroFE") > 0, "Measure text using migrated glyph metrics");
        const std::string accented = "caf\xc3\xa9";
        const int coldWidth = font.getWidth(accented);
        auto* mip = const_cast<FontManager::MipLevel*>(font.getMipLevelForSize(24));
        require(mip->dynamicGlyphs.empty(), "Measurement does not upload glyphs");
        require(font.loadGlyphOnDemand(0xe9, mip), "Load accented Latin glyph");
        require(coldWidth == font.getWidth(accented), "Width is independent of atlas population");
        const auto generation = font.getResourceGeneration();
        font.deInitialize();
        require(font.getResourceGeneration() != generation, "Font teardown invalidates cached resources");
        require(font.getWidth(accented) == 0, "Deinitialized font has no stale metrics");
        require(font.initialize(), "Rebuild font resources");
        require(font.getWidth(accented) == coldWidth, "Font reload retains measurement");
        require(font.getWidth(std::string("A\0B", 3)) == font.getWidth("A"), "Embedded NUL terminates measurement");
        require(font.getWidth("A\xc3") > 0, "Truncated UTF-8 is handled safely");

        Configuration pageConfig;
        Page page(pageConfig, 64, 64);
        Text label(accented, page, &font, 0);
        label.baseViewInfo.FontSize = 24;
        label.draw();
        require(font.getMipLevelForSize(24)->dynamicGlyphs.count(0xe9) == 1,
            "Text renders accented Latin outside the ASCII preload");
        require(font.initialize(), "Reload font used by existing text");
        label.draw();
        require(font.getMipLevelForSize(24)->dynamicGlyphs.count(0xe9) == 1,
            "Text rebuilds glyph cache after resource reload");
        FontManager larger(assets + "/retrofe/OpenSans.ttf", 48, {255,255,255,255}, true, 1, 0);
        require(larger.initialize(), "Prepare second font at layout load time");
        label.baseViewInfo.font = &larger;
        label.baseViewInfo.FontSize = 48; // The scale remains 1.0 across this switch.
        label.draw();
        require(larger.getMipLevelForSize(48)->dynamicGlyphs.count(0xe9) == 1,
            "Text rebuilds glyph cache when font changes at the same scale");
        label.baseViewInfo.font = &font;
        {
            FontManager mipped(assets + "/retrofe/OpenSans.ttf", 96, {255,255,255,255}, true, 4, 0);
            require(mipped.initialize(), "Prebuild mip ladder");
            for (int size : {12, 18, 24, 36, 48, 72, 96}) {
                const auto* level = mipped.getMipLevelForSize(float(size));
                require(level && level->fontSize == size && level->font && level->fillTexture && level->outlineTexture,
                    "Each mip owns its font and styled atlas");
                require(level->outlinePx == std::max(1, int(std::lround(4.0 * size / 96))),
                    "Outline thickness follows raster size");
            }
            require(mipped.getMipLevelForSize(24.1f)->fontSize == 36, "Fractional requests select the ceiling mip");
            require(mipped.getMipLevelForSize(24)->dynamicFillTexture->w == 512 &&
                mipped.getMipLevelForSize(96)->dynamicFillTexture->w == 2048,
                "Unicode atlases are preallocated proportionately to mip size");
            require(mipped.getMipLevelForSize(1)->fontSize == 12, "Small sizes use the minimum prepared mip");
            require(mipped.getMipLevelForSize(120)->fontSize == 96, "Selection does not open unprepared fonts");
            require(mipped.prepareSize(26) && mipped.prepareSize(34), "Prepare sizes found in active layouts");
            require(mipped.getMipLevelForSize(26)->fontSize == 26 && mipped.getMipLevelForSize(34)->fontSize == 34,
                "Exact layout sizes override ladder choices");
            label.baseViewInfo.font = &mipped;
            label.baseViewInfo.FontSize = 26;
            label.draw();
            label.baseViewInfo.FontSize = 34;
            label.draw();
            require(mipped.getMipLevelForSize(26)->dynamicGlyphs.count(0xe9) == 1 &&
                mipped.getMipLevelForSize(34)->dynamicGlyphs.count(0xe9) == 1,
                "Text changes mip at constant scale and uses each mip's Unicode atlas");
            label.baseViewInfo.font = &font;
            require(mipped.prepareSize(120), "Prepare an explicit size above loadFontSize");
            require(mipped.getMaxFontSize() == 96, "Reference size remains stable");
            require(mipped.prepareHeight(65), "Prepare hiscore line height");
            const auto* level = mipped.getMipLevelForHeight(65);
            require(level && level->height >= 65, "Height selector avoids upscaling");
            int advance = 0, kerning = 0;
            require(TTF_GetGlyphMetrics(level->font, 'A', nullptr, nullptr, nullptr, nullptr, &advance), "Read selected metrics");
            require(TTF_GetGlyphKerning(level->font, 'A', 'A', &kerning), "Read selected kerning");
            require(mipped.getWidth("AA", *level) == advance * 2 + kerning + level->outlinePx * 2,
                "Measurement uses selected font and outline");
            const int heightSize = level->fontSize;
            mipped.deInitialize();
            require(mipped.initialize(), "Rebuild all prepared sizes after renderer reload");
            require(mipped.getMipLevelForSize(26)->fontSize == 26 && mipped.getMipLevelForSize(120)->fontSize == 120 &&
                mipped.getMipLevelForHeight(65)->fontSize == heightSize, "Exact sizes survive resource reload");
        }
        {
            FontCache cache;
            require(cache.initialize(), "Initialize layout font cache");
            const auto path = assets + "/retrofe/OpenSans.ttf";
            require(cache.loadFont(path, 48, {255,255,255,255}, false, 0, 0) &&
                cache.loadFont(path, 24, {255,255,255,255}, false, 0, 0), "Load larger layout before smaller layout");
            require(cache.getFont(path, 24, false, 0, 0)->getMaxFontSize() == 24,
                "Layout default size is independent of previously loaded larger fonts");
        }
    }
    TTF_Quit();
    gst_init(nullptr, nullptr);
    for (const char* name : {"playbin", "appsink", "appsrc", "audioconvert",
            "audioresample", "videoconvert", "perspective", "goom",
            "wavescope", "synaescope", "spectrascope"}) {
        auto* factory = gst_element_factory_find(name);
        require(factory != nullptr, name);
        gst_object_unref(factory);
    }
    GlibLoop::instance().start();
    {
        auto video = std::make_shared<GStreamerVideo>(0);
        require(video->open(assets + "/layouts/Arcades/video/splash.mp4"), "Open packaged video");
        const Uint64 deadline = SDL_GetTicks() + 10000;
        while (!video->getTexture() && !video->hasError() && SDL_GetTicks() < deadline) {
            video->updateFrame();
            SDL_Delay(10);
        }
        require(video->getTexture() != nullptr, "Decode video into an SDL3 texture");
        if (Configuration::HardwareVideoAccel) require(video->usingGpuTexture(), "Hardware test must use native GPU texture interop");
        std::cout << "Video path: " << (video->usingGpuTexture() ? "native GPU texture" : "CPU upload") << '\n';
        video->resume();
        const Uint64 playbackDeadline = SDL_GetTicks() + 2000;
        unsigned presented = 0;
        while (SDL_GetTicks() < playbackDeadline) {
            video->updateFrame();
            auto* renderer = SDL::getRenderer(0);
            D3D11RenderLock renderLock(renderer);
            require(SDL_SetRenderTarget(renderer, nullptr), "Set video backbuffer");
            const auto dim = video->getDimensions();
            SDL_FRect source{0, 0, float(dim.w), float(dim.h)};
            require(SDL_RenderTexture(renderer, video->getTexture(), &source, nullptr), "Render decoded video texture");
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
            SDL_FRect overlay{4, 4, 8, 8};
            require(SDL_RenderFillRect(renderer, &overlay), "Draw overlay above video");
            require(pixel(renderer, 6, 6).r == 255, "Video preserves overlay rendering");
            renderLock.finish();
            require(SDL_RenderPresent(renderer), "Present decoded video");
            D3D11VideoInterop::presented(renderer);
            ++presented;
            SDL_Delay(10);
        }
        require(presented > 10, "Repeated video frames render successfully");
        if (Configuration::HardwareVideoAccel) require(video->gpuFrameCount() > 10, "Decode and copy multiple distinct GPU frames");
        video->pause();
        video->resume();
        require(video->unload(), "Unload video for instance reuse");
        const Uint64 unloadDeadline = SDL_GetTicks() + 8000;
        while (!video->isReadyForReuse() && SDL_GetTicks() < unloadDeadline) SDL_Delay(10);
        require(video->isReadyForReuse(), "Video instance drains for reuse");
        require(video->open(assets + "/layouts/Arcades/video/splash.mp4"), "Reopen video on retained instance");
        const Uint64 reopenDeadline = SDL_GetTicks() + 10000;
        while (!video->getTexture() && !video->hasError() && SDL_GetTicks() < reopenDeadline) {
            video->updateFrame();
            SDL_Delay(10);
        }
        require(video->getTexture() != nullptr, "Decode after instance reuse");
        if (Configuration::HardwareVideoAccel) require(video->usingGpuTexture(), "GPU interop survives instance reuse");
        video->stop();
    }
    GlibLoop::instance().stop();
}
}

int main(int argc, char** argv) {
    const bool hardware = argc > 2 && std::string(argv[2]) == "--hardware";
    if (!hardware) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
#ifdef _WIN32
    const char* hardwareRenderer = "direct3d11";
#else
    const char* hardwareRenderer = std::getenv("RETROFE_TEST_RENDERER");
    if (!hardwareRenderer) hardwareRenderer = "opengl";
#endif
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Configuration config;
    config.setProperty("log", std::string("INFO,WARNING,ERROR"));
    require(Logger::initialize(hardware ? "sdl3-hardware-runtime.log" : "sdl3-software-runtime.log", &config), "Initialize runtime log");
    config.setProperty("SDLRenderDriver", std::string(hardware ? hardwareRenderer : "software"));
    config.setProperty("HardwareVideoAccel", hardware);
    config.setProperty("screenOrder", std::string("0"));
    config.setProperty("horizontal0", 64);
    config.setProperty("vertical0", 64);
    config.setProperty("fullscreen0", false);
    config.setProperty("hideMouse", false);
    config.setProperty("vSync", false);
    config.setProperty("layoutScaleMode", std::string("stretch"));
    renderChecks(config, true);
    inputChecks(config);
    if (argc > 1) mediaChecks(argv[1]);
    require(SDL::deInitialize(false), "Unload video while retaining audio/input");
    renderChecks(config, true);
    require(SDL::deInitialize(true), "Full SDL shutdown");
    config.setProperty("mirror0", true);
    config.setProperty("rotation0", 1);
    config.setProperty("layoutScaleMode", std::string("fit"));
    renderChecks(config, false);
    require(SDL::deInitialize(true), "Shutdown mirrored rotated output");
    std::cout << "SDL3 rendering and lifecycle smoke tests passed\n";
    Logger::deInitialize();
    return EXIT_SUCCESS;
}
