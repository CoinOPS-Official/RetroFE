#include "../SDL.h"
#include "../Database/Configuration.h"
#include "../Control/UserInput.h"
#include "../Sound/AudioBus.h"
#include "../Sound/Sound.h"
#include "../Graphics/Font.h"
#include "../Video/GStreamerVideo.h"
#include "../Video/GlibLoop.h"
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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
    ViewInfo view;
    SDL_FRect dest{8, 8, 16, 16};
    require(SDL::renderCopyF(texture, 0.5f, nullptr, &dest, view, 64, 64), "Draw tinted alpha geometry");
    if (checkPixels) {
        const auto color = pixel(renderer, 16, 16);
        require(color.r >= 95 && color.r <= 105 && color.g >= 45 && color.g <= 55 && color.b >= 20 && color.b <= 30,
            "SDL3 float vertex colors preserve tint and alpha");
        require(pixel(renderer, 2, 2).r == 0, "Geometry stays inside destination");
    }
    require(SDL_RenderClear(renderer), "Clear for reflection");
    view.hasReflection = true;
    view.reflectionMask = 2;
    view.ReflectionScale = 1.0f;
    view.ReflectionAlpha = 0.5f;
    require(SDL::renderCopyF(texture, 1.0f, nullptr, &dest, view, 64, 64), "Draw reflection");
    if (checkPixels) {
        require(pixel(renderer, 16, 16).r >= 195, "Primary image remains opaque");
        const auto reflected = pixel(renderer, 16, 32);
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
        require(pixel(renderer, 12, 16).r == 0, "Container clips left side");
        require(pixel(renderer, 20, 16).r >= 195, "Container preserves visible side");
    }
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
