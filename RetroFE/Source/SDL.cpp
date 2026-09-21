/* This file is part of RetroFE.
*
* RetroFE is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* RetroFE is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "SDL.h"
#include "Video/VideoFactory.h"
#ifdef RETROFE_HAVE_D3D12
#include "Video/D3D12VideoInterop.h"
#endif
#include "Graphics/GeometryBatch.h"
#include "Database/Configuration.h"
#include "Database/GlobalOpts.h"
#include "Utility/Log.h"
#include "Sound/AudioBus.h"
#include "Sound/MusicPlayer.h"

#include "Utility/Utils.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>

std::vector<SDL_Window*>    SDL::window_;
std::vector<SDL_Renderer*>  SDL::renderer_;
static std::vector<SDL_Texture*> renderTargets_;
std::vector<int>            SDL::displayWidth_;
std::vector<int>            SDL::displayHeight_;
std::vector<int>            SDL::windowWidth_;
std::vector<int>            SDL::windowHeight_;
std::vector<int>            SDL::displayRefresh_;
std::vector<bool>           SDL::fullscreen_;
std::vector<int>            SDL::rotation_;
std::vector<bool>           SDL::mirror_;
int                         SDL::numScreens_ = 1;
int                         SDL::numDisplays_ = 1;
int                         SDL::screenCount_;
SDL::LayoutScaleMode SDL::layoutScaleMode_ = SDL::LayoutScaleMode::Stretch;

static bool configureFullscreen(
    SDL_Window* window,
    SDL_DisplayID displayID,
    bool exclusive,
    int requestedWidth,
    int requestedHeight,
    float requestedRefresh) {
    if (!window) {
        LOG_ERROR("SDL", "configureFullscreen called with null window");
        return false;
    }

    if (exclusive) {
        SDL_DisplayMode closestMode{};

        if (!SDL_GetClosestFullscreenDisplayMode(
            displayID,
            requestedWidth,
            requestedHeight,
            requestedRefresh,
            false,
            &closestMode))
        {
            LOG_ERROR(
                "SDL",
                "Unable to find fullscreen mode near " +
                std::to_string(requestedWidth) + "x" +
                std::to_string(requestedHeight) + "@" +
                std::to_string(requestedRefresh) + " Hz: " +
                std::string(SDL_GetError())
            );
            return false;
        }

        LOG_INFO(
            "SDL",
            "Using exclusive fullscreen mode " +
            std::to_string(closestMode.w) + "x" +
            std::to_string(closestMode.h) + "@" +
            std::to_string(closestMode.refresh_rate) + " Hz"
        );

        if (!SDL_SetWindowFullscreenMode(window, &closestMode)) {
            LOG_ERROR(
                "SDL",
                "Unable to set exclusive fullscreen mode: " +
                std::string(SDL_GetError())
            );
            return false;
        }
    }
    else {
        // SDL3: nullptr explicitly selects borderless fullscreen desktop.
        if (!SDL_SetWindowFullscreenMode(window, nullptr)) {
            LOG_ERROR(
                "SDL",
                "Unable to select borderless fullscreen desktop mode: " +
                std::string(SDL_GetError())
            );
            return false;
        }

        LOG_INFO("SDL", "Using borderless fullscreen desktop mode");
    }

    if (!SDL_SetWindowFullscreen(window, true)) {
        LOG_ERROR(
            "SDL",
            "Unable to enter fullscreen: " +
            std::string(SDL_GetError())
        );
        return false;
    }

    // Fullscreen/window-state transitions may be asynchronous,
    // particularly under Wayland. We need the final drawable size
    // before creating the renderer target.
    if (!SDL_SyncWindow(window)) {
        LOG_WARNING(
            "SDL",
            "Unable to synchronize fullscreen window state: " +
            std::string(SDL_GetError())
        );
    }

    return true;
}

// Initialize SDL
bool SDL::initialize(Configuration& config) {
    int audioRate = 48000;
    int audioChannels = 2;
    bool hideMouse = false;

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

#ifdef WIN32
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
    {
        LOG_ERROR("SDL", "Unable to set DPI awareness hint");
    }
#endif

    if (SDL_WasInit(0) == 0) {
        // First-time startup: Initialize everything.
        LOG_INFO(
            "SDL",
            "Performing first-time full initialization of all SDL subsystems."
        );

        if (!SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_GAMEPAD |
            SDL_INIT_AUDIO))
        {
            std::string error = SDL_GetError();
            LOG_ERROR("SDL", "Initial SDL_Init failed: " + error);
            return false;
        }
    }
    else {
        // Re-initialization: Audio and input are already running.
        // Only initialize video.
        //
        // Robust retry logic for the Pi5 race condition.
        LOG_INFO("SDL", "Attempting to re-initialize video subsystem...");

        const int MAX_RETRIES = 10;
        const int RETRY_DELAY_MS = 100;
        bool success = false;

        for (int i = 0; i < MAX_RETRIES; ++i) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                success = true;

                LOG_INFO(
                    "SDL",
                    "Video subsystem re-initialized successfully on attempt " +
                    std::to_string(i + 1) + "."
                );

                break;
            }

            LOG_WARNING(
                "SDL",
                "Failed to re-initialize video subsystem (attempt " +
                std::to_string(i + 1) + "/" +
                std::to_string(MAX_RETRIES) + "): " +
                std::string(SDL_GetError()) +
                ". Retrying..."
            );

            SDL_Delay(RETRY_DELAY_MS);
        }

        if (!success) {
            LOG_ERROR(
                "SDL",
                "Failed to re-initialize video subsystem after " +
                std::to_string(MAX_RETRIES) +
                " attempts. Giving up."
            );

            return false;
        }
    }

    std::string SDLRenderDriver;
    config.getProperty(OPTION_SDLRENDERDRIVER, SDLRenderDriver);

#ifdef WIN32
    if (SDLRenderDriver == "direct3d")
        SDLRenderDriver = "direct3d11";
#endif

#ifdef RETROFE_HAVE_D3D12
    if (SDLRenderDriver.empty()) SDLRenderDriver = "direct3d12,direct3d11";
#endif

    if (SDLRenderDriver.empty()) {
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    }
    else if (!SDL_SetHint(
        SDL_HINT_RENDER_DRIVER,
        SDLRenderDriver.c_str()))
    {
        LOG_WARNING(
            "SDL",
            "Unable to select renderer " +
            SDLRenderDriver + ": " +
            SDL_GetError()
        );
    }

    std::string ScaleQuality = "1";
    config.getProperty(OPTION_SCALEQUALITY, ScaleQuality);

    // SDL3 selects texture filtering per renderer/texture;
    // batching is automatic.
    const SDL_ScaleMode scaleMode =
        (ScaleQuality == "0" || ScaleQuality == "nearest")
        ? SDL_SCALEMODE_NEAREST
        : SDL_SCALEMODE_LINEAR;

    std::string layoutScaleModeString = "stretch";
    config.getProperty(
        OPTION_LAYOUTSCALEMODE,
        layoutScaleModeString
    );

    std::transform(
        layoutScaleModeString.begin(),
        layoutScaleModeString.end(),
        layoutScaleModeString.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    if (layoutScaleModeString == "fit") {
        layoutScaleMode_ = LayoutScaleMode::Fit;
    }
    else if (layoutScaleModeString == "fill") {
        layoutScaleMode_ = LayoutScaleMode::Fill;
    }
    else {
        if (layoutScaleModeString != "stretch") {
            LOG_WARNING(
                "SDL",
                "Invalid layoutScaleMode '" +
                layoutScaleModeString +
                "'. Valid values are stretch, fit, and fill. "
                "Defaulting to stretch."
            );
        }

        layoutScaleMode_ = LayoutScaleMode::Stretch;
    }

    LOG_INFO(
        "SDL",
        "Layout scale mode: " +
        std::string(
            layoutScaleMode_ == LayoutScaleMode::Fill
            ? "fill"
            : layoutScaleMode_ == LayoutScaleMode::Fit
            ? "fit"
            : "stretch"
        )
    );

    if (config.getProperty(OPTION_HIDEMOUSE, hideMouse))
        hideMouse ? SDL_HideCursor() : SDL_ShowCursor();

    // ---------------------------------------------------------
    // Configuration for hardware/video/audio
    // ---------------------------------------------------------

    bool HardwareVideoAccel = false;
    config.getProperty(
        OPTION_HARDWAREVIDEOACCEL,
        HardwareVideoAccel
    );

    Configuration::HardwareVideoAccel = HardwareVideoAccel;
    std::string videoBackend = "gstreamer";
    config.getProperty("VideoBackend", videoBackend);
    if (!VideoFactory::setBackend(videoBackend)) return false;

    // Hardware video shares the D3D11 device with
    // GStreamer streaming threads.
    SDL_SetHint(
        SDL_HINT_RENDER_DIRECT3D_THREADSAFE,
        "1"
    );

    int AvdecMaxThreads = 2;
    config.getProperty(
        OPTION_AVDECMAXTHREADS,
        AvdecMaxThreads
    );

    Configuration::AvdecMaxThreads = AvdecMaxThreads;

    int AvdecThreadType = 2;
    config.getProperty(
        OPTION_AVDECTHREADTYPE,
        AvdecThreadType
    );

    Configuration::AvdecThreadType = AvdecThreadType;

    bool MuteVideo = false;
    config.getProperty(
        OPTION_MUTEVIDEO,
        MuteVideo
    );

    Configuration::MuteVideo = MuteVideo;

    // Apply this before any window enters fullscreen.
    bool minimizeOnFocusLoss = false;

    if (config.getProperty(
        OPTION_MINIMIZEONFOCUSLOSS,
        minimizeOnFocusLoss))
    {
        SDL_SetHintWithPriority(
            SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
            minimizeOnFocusLoss ? "1" : "0",
            SDL_HINT_OVERRIDE
        );
    }

    // ---------------------------------------------------------
    // Parse screenOrder with backwards compatibility
    // ---------------------------------------------------------

    std::string screenOrderStr;

    if (config.propertyExists(OPTION_SCREENORDER)) {
        config.getProperty(
            OPTION_SCREENORDER,
            screenOrderStr
        );

        LOG_INFO(
            "SDL",
            "Using configured screenOrder: " +
            screenOrderStr
        );
    }
    else {
        // Fallback: use legacy screenNumX or numScreens.
        int numScreens = -1;
        config.getProperty("numScreens", numScreens);

        if (numScreens > 0) {
            for (int i = 0; i < numScreens; ++i) {
                int screenNum = i;

                config.getProperty(
                    "screenNum" + std::to_string(i),
                    screenNum
                );

                if (!screenOrderStr.empty())
                    screenOrderStr += ",";

                screenOrderStr += std::to_string(screenNum);
            }

            LOG_INFO(
                "SDL",
                "No screenOrder specified. Using screenNumX and "
                "numScreens: " +
                screenOrderStr
            );
        }
        else {
            // Auto-detect as fallback.
            std::vector<int> legacyScreenNums;

            for (int i = 0;; ++i) {
                std::string key =
                    "screenNum" + std::to_string(i);

                int val;

                if (config.getProperty(key, val)) {
                    legacyScreenNums.push_back(val);
                }
                else {
                    break;
                }
            }

            if (!legacyScreenNums.empty()) {
                for (size_t i = 0;
                    i < legacyScreenNums.size();
                    ++i)
                {
                    if (i > 0)
                        screenOrderStr += ",";

                    screenOrderStr +=
                        std::to_string(
                            legacyScreenNums[i]
                        );
                }

                LOG_INFO(
                    "SDL",
                    "No screenOrder or numScreens specified. "
                    "Using detected screenNumX: " +
                    screenOrderStr
                );
            }
            else {
                screenOrderStr = "0";

                LOG_WARNING(
                    "SDL",
                    "No screenOrder, screenNumX, or numScreens "
                    "specified. Defaulting to screen 0."
                );
            }
        }
    }

    // Split and convert to vector<int>.
    std::vector<std::string> screenOrderStrVec;

    Utils::listToVector(
        screenOrderStr,
        screenOrderStrVec,
        ','
    );

    std::vector<int> screenOrder;

    for (const auto& s : screenOrderStrVec) {
        try {
            int idx = std::stoi(s);
            screenOrder.push_back(idx);
        }
        catch (...) {
            LOG_WARNING(
                "SDL",
                "Invalid entry in screenOrder: '" +
                s +
                "' (not an integer). Ignored."
            );
        }
    }

    int numDisplays = 0;

    std::unique_ptr<
        SDL_DisplayID,
        decltype(&SDL_free)
    > displays(
        SDL_GetDisplays(&numDisplays),
        SDL_free
    );

    if (!displays || numDisplays < 1) {
        LOG_ERROR(
            "SDL",
            "No SDL video displays detected."
        );

        return false;
    }

    // ---------------------------------------------------------
    // Validate and filter screenOrder
    // ---------------------------------------------------------

    std::vector<int> validScreenOrder;

    for (auto displayIndex : screenOrder) {
        if (displayIndex < numDisplays &&
            displayIndex >= 0)
        {
            validScreenOrder.push_back(
                displayIndex
            );
        }
        else {
            LOG_WARNING(
                "SDL",
                "screenOrder entry " +
                std::to_string(displayIndex) +
                " ignored (only " +
                std::to_string(numDisplays) +
                " displays present)."
            );
        }
    }

    if (validScreenOrder.empty()) {
        LOG_ERROR(
            "SDL",
            "No valid displays listed in screenOrder! "
            "Initialization aborted."
        );

        return false;
    }

    screenOrder = validScreenOrder;

    numDisplays_ = numDisplays;
    numScreens_ =
        static_cast<int>(screenOrder.size());

    screenCount_ =
        static_cast<int>(screenOrder.size());

    LOG_INFO(
        "SDL",
        "Number of displays found: " +
        std::to_string(numDisplays)
    );

    LOG_INFO(
        "SDL",
        "Number of screens requested: " +
        std::to_string(screenCount_)
    );

    // ---------------------------------------------------------
    // OpenGL system RAM optimizations
    // ---------------------------------------------------------

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE
    );

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MAJOR_VERSION,
        3
    );

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MINOR_VERSION,
        2
    );

    // No depth/stencil backing allocation required.
    SDL_GL_SetAttribute(
        SDL_GL_DEPTH_SIZE,
        0
    );

    SDL_GL_SetAttribute(
        SDL_GL_STENCIL_SIZE,
        0
    );

    // ---------------------------------------------------------
    // Per-screen initialization
    // ---------------------------------------------------------

    for (int logicalScreen = 0;
        logicalScreen < screenCount_;
        ++logicalScreen)
    {
        const int physicalDisplay =
            screenOrder[logicalScreen];

        const SDL_DisplayID displayID =
            displays.get()[physicalDisplay];

        const SDL_DisplayMode* mode =
            SDL_GetCurrentDisplayMode(displayID);

        bool windowBorder = false;
        bool windowResize = false;

        // Do not encode fullscreen behavior into the creation
        // flags. SDL_SetWindowFullscreenMode() below controls
        // exclusive vs borderless fullscreen.
        SDL_WindowFlags windowFlags = 0;

        std::string screenIndex =
            std::to_string(logicalScreen);

        config.getProperty(
            OPTION_WINDOWBORDER,
            windowBorder
        );

        if (!windowBorder)
            windowFlags |= SDL_WINDOW_BORDERLESS;

        config.getProperty(
            OPTION_WINDOWRESIZE,
            windowResize
        );

        if (windowResize)
            windowFlags |= SDL_WINDOW_RESIZABLE;

        if (!mode) {
            LOG_ERROR(
                "SDL",
                "Cannot query display " +
                std::to_string(physicalDisplay) +
                ": " +
                SDL_GetError()
            );

            return false;
        }

        displayRefresh_.push_back(
            static_cast<int>(
                std::lround(mode->refresh_rate)
                )
        );

        windowWidth_.push_back(mode->w);
        displayWidth_.push_back(mode->w);

        // -----------------------------------------------------
        // Horizontal resolution
        // -----------------------------------------------------

        std::string hString = "";

        if (logicalScreen == 0)
            config.getProperty(
                OPTION_HORIZONTAL,
                hString
            );

        config.getProperty(
            OPTION_HORIZONTAL + screenIndex,
            hString
        );

        if (hString == "")
        {
            LOG_ERROR(
                "Configuration",
                "Missing property \"horizontal\"" +
                screenIndex
            );

            return false;
        }
        else if (hString == "envvar")
        {
            hString =
                Utils::getEnvVar(
                    "H_RES_" + screenIndex
                );

            if (hString == "" ||
                !Utils::convertInt(hString))
            {
                LOG_WARNING(
                    "Configuration",
                    "Invalid property value for \"horizontal\"" +
                    screenIndex +
                    " defaulted to 'stretch'"
                );
            }
            else
            {
                LOG_WARNING(
                    "Configuration",
                    "H_RES_" +
                    screenIndex +
                    " for \"horizontal\" set to " +
                    hString
                );

                windowWidth_[logicalScreen] =
                    Utils::convertInt(hString);
            }
        }
        else if (
            hString != "stretch" &&
            (
                logicalScreen != 0 ||
                !config.getProperty(
                    OPTION_HORIZONTAL,
                    windowWidth_[logicalScreen]
                )
                ) &&
            !config.getProperty(
                OPTION_HORIZONTAL + screenIndex,
                windowWidth_[logicalScreen]
            ))
        {
            LOG_ERROR(
                "Configuration",
                "Invalid property value for \"horizontal\"" +
                screenIndex
            );

            return false;
        }

        // -----------------------------------------------------
        // Vertical resolution
        // -----------------------------------------------------

        windowHeight_.push_back(mode->h);
        displayHeight_.push_back(mode->h);

        std::string vString = "";

        if (logicalScreen == 0)
            config.getProperty(
                OPTION_VERTICAL,
                vString
            );

        config.getProperty(
            OPTION_VERTICAL + screenIndex,
            vString
        );

        if (vString == "")
        {
            LOG_ERROR(
                "Configuration",
                "Missing property \"vertical\"" +
                screenIndex
            );

            return false;
        }
        else if (vString == "envvar")
        {
            vString =
                Utils::getEnvVar(
                    "V_RES_" + screenIndex
                );

            if (vString == "" ||
                !Utils::convertInt(vString))
            {
                LOG_WARNING(
                    "Configuration",
                    "Invalid property value for \"vertical\"" +
                    screenIndex +
                    " defaulted to 'stretch'"
                );
            }
            else
            {
                LOG_WARNING(
                    "Configuration",
                    "V_RES_" +
                    screenIndex +
                    " for \"vertical\" set to " +
                    vString
                );

                windowHeight_[logicalScreen] =
                    Utils::convertInt(vString);
            }
        }
        else if (
            vString != "stretch" &&
            (
                logicalScreen != 0 ||
                !config.getProperty(
                    OPTION_VERTICAL,
                    windowHeight_[logicalScreen]
                )
                ) &&
            !config.getProperty(
                OPTION_VERTICAL + screenIndex,
                windowHeight_[logicalScreen]
            ))
        {
            LOG_ERROR(
                "Configuration",
                "Invalid property value for \"vertical\"" +
                screenIndex
            );

            return false;
        }

        // -----------------------------------------------------
        // Fullscreen configuration
        //
        // RetroFE semantics:
        //
        //   fullscreen=true
        //       explicit display mode / exclusive fullscreen
        //
        //   fullscreen=false
        //       borderless fullscreen desktop
        //
        // A per-screen property overrides the global property.
        // -----------------------------------------------------

        bool fullscreen = false;

        const bool hasGlobalFullscreen =
            config.getProperty(
                OPTION_FULLSCREEN,
                fullscreen
            );

        bool perScreenFullscreen = fullscreen;

        const bool hasPerScreenFullscreen =
            config.getProperty(
                OPTION_FULLSCREEN + screenIndex,
                perScreenFullscreen
            );

        if (hasPerScreenFullscreen)
            fullscreen = perScreenFullscreen;

        if (!hasGlobalFullscreen &&
            !hasPerScreenFullscreen)
        {
            LOG_ERROR(
                "Configuration",
                "Missing property: \"fullscreen\"" +
                screenIndex
            );

            return false;
        }

        fullscreen_.push_back(fullscreen);

        int rotation = 0;

        config.getProperty(
            OPTION_ROTATION + screenIndex,
            rotation
        );

        LOG_INFO(
            "Configuration",
            "Setting rotation for screen " +
            screenIndex +
            " to " +
            std::to_string(rotation * 90) +
            " degrees."
        );

        rotation_.push_back(rotation);

        bool mirror = false;

        config.getProperty(
            OPTION_MIRROR + screenIndex,
            mirror
        );

        if (mirror) {
            LOG_INFO(
                "Configuration",
                "Setting mirror mode for screen " +
                screenIndex + "."
            );
        }

        mirror_.push_back(mirror);

        window_.push_back(nullptr);
        renderer_.push_back(nullptr);

        const std::string fullscreenStr =
            fullscreen_[logicalScreen]
            ? "exclusive"
            : "borderless desktop";

        std::stringstream ss;

        ss
            << "Creating "
            << windowWidth_[logicalScreen]
            << "x"
            << windowHeight_[logicalScreen]
            << " window (fullscreen mode: "
            << fullscreenStr
            << ") for logical screen "
            << logicalScreen
            << " on physical display "
            << physicalDisplay;

        LOG_INFO("SDL", ss.str());

        std::string retrofeTitle =
            "RetroFE " +
            std::to_string(physicalDisplay);

        // -----------------------------------------------------
        // Create window
        // -----------------------------------------------------

        if (!window_[logicalScreen])
        {
            const SDL_PropertiesID props =
                SDL_CreateProperties();

            if (!props)
                return false;

            const bool configured =
                SDL_SetStringProperty(
                    props,
                    SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                    retrofeTitle.c_str()
                ) &&
                SDL_SetNumberProperty(
                    props,
                    SDL_PROP_WINDOW_CREATE_X_NUMBER,
                    SDL_WINDOWPOS_CENTERED_DISPLAY(
                        displayID
                    )
                ) &&
                SDL_SetNumberProperty(
                    props,
                    SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                    SDL_WINDOWPOS_CENTERED_DISPLAY(
                        displayID
                    )
                ) &&
                SDL_SetNumberProperty(
                    props,
                    SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                    windowWidth_[logicalScreen]
                ) &&
                SDL_SetNumberProperty(
                    props,
                    SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                    windowHeight_[logicalScreen]
                ) &&
                SDL_SetNumberProperty(
                    props,
                    SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                    windowFlags
                );

            if (configured) {
                window_[logicalScreen] =
                    SDL_CreateWindowWithProperties(
                        props
                    );
            }

            SDL_DestroyProperties(props);
        }

        if (window_[logicalScreen] == nullptr)
        {
            std::string error = SDL_GetError();

            if (logicalScreen == 0)
            {
                LOG_ERROR(
                    "SDL",
                    "Create window " +
                    screenIndex +
                    " on display " +
                    std::to_string(
                        physicalDisplay
                    ) +
                    " failed: " +
                    error
                );

                return false;
            }
            else
            {
                LOG_WARNING(
                    "SDL",
                    "Create window " +
                    screenIndex +
                    " on display " +
                    std::to_string(
                        physicalDisplay
                    ) +
                    " failed: " +
                    error
                );

                continue;
            }
        }

        // -----------------------------------------------------
        // Configure fullscreen state
        // -----------------------------------------------------

        if (!configureFullscreen(
            window_[logicalScreen],
            displayID,
            fullscreen_[logicalScreen],
            windowWidth_[logicalScreen],
            windowHeight_[logicalScreen],
            mode->refresh_rate))
        {
            return false;
        }

        // -----------------------------------------------------
        // Mouse setup
        // -----------------------------------------------------

        if (logicalScreen == 0)
        {
#ifndef __APPLE__
            SDL_WarpMouseInWindow(
                window_[logicalScreen],
                windowWidth_[logicalScreen],
                0
            );
#else
            SDL_WarpMouseInWindow(
                window_[logicalScreen],
                windowWidth_[logicalScreen] / 2,
                windowHeight_[logicalScreen] / 2
            );
#endif

            SDL_SetWindowRelativeMouseMode(
                window_[logicalScreen],
                hideMouse
            );
        }

        // -----------------------------------------------------
        // Renderer
        // -----------------------------------------------------

        bool vSync = false;
        config.getProperty(
            OPTION_VSYNC,
            vSync
        );

        if (!renderer_[logicalScreen])
        {
            renderer_[logicalScreen] =
                SDL_CreateRenderer(
                    window_[logicalScreen],
                    nullptr
                );
        }

        if (renderer_[logicalScreen] == nullptr)
        {
            std::string error = SDL_GetError();

            LOG_ERROR(
                "SDL",
                "Create renderer " +
                screenIndex +
                " failed: " +
                error
            );

            return false;
        }
        else
        {
#ifdef RETROFE_HAVE_GST_GL
            const char* backendName =
                SDL_GetRendererName(
                    renderer_[logicalScreen]
                );

            if (backendName &&
                (
                    std::string(backendName) == "opengl" ||
                    std::string(backendName) == "opengles2"
                    ))
            {
                SDL_SetPointerProperty(
                    SDL_GetRendererProperties(
                        renderer_[logicalScreen]
                    ),
                    "retrofe.gl.context",
                    SDL_GL_GetCurrentContext()
                );
            }
#endif

            if (!SDL_SetRenderVSync(
                renderer_[logicalScreen],
                vSync ? 1 : 0))
            {
                LOG_WARNING(
                    "SDL",
                    "Unable to set renderer vsync: " +
                    std::string(SDL_GetError())
                );
            }

            SDL_SetDefaultTextureScaleMode(
                renderer_[logicalScreen],
                scaleMode
            );

            // Fullscreen has been synchronized above.
            // Query the actual drawable dimensions rather
            // than assuming the requested window size.
            if (!SDL_GetRenderOutputSize(
                renderer_[logicalScreen],
                &windowWidth_[logicalScreen],
                &windowHeight_[logicalScreen]))
            {
                return false;
            }

            // Ensure vector is sized for all screens.
            renderTargets_.resize(
                screenCount_,
                nullptr
            );

            // -------------------------------------------------
            // Create offscreen compositing render target
            // -------------------------------------------------

            {
                SDL_Renderer* r =
                    renderer_[logicalScreen];

                if (!r)
                    return false;

                const int w =
                    windowWidth_[logicalScreen];

                const int h =
                    windowHeight_[logicalScreen];

                SDL_Texture* t =
                    SDL_CreateTexture(
                        r,
                        SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_TARGET,
                        w,
                        h
                    );

                if (!t) {
                    LOG_ERROR(
                        "SDL",
                        "Failed to create render target texture: " +
                        std::string(SDL_GetError())
                    );

                    return false;
                }

                // Standard alpha blend mode for compositing UI.
                SDL_SetTextureBlendMode(
                    t,
                    SDL_BLENDMODE_BLEND
                );

                SDL_SetTextureScaleMode(
                    t,
                    SDL_SCALEMODE_LINEAR
                );

                // One-time clear so texture contents are defined.
                SDL_SetRenderTarget(r, t);

                SDL_SetRenderDrawColor(
                    r,
                    0,
                    0,
                    0,
                    255
                );

                SDL_RenderClear(r);

                SDL_SetRenderTarget(
                    r,
                    nullptr
                );

                renderTargets_[logicalScreen] = t;
            }

            // -------------------------------------------------
            // Renderer logging / backend-specific settings
            // -------------------------------------------------

            const char* backend =
                SDL_GetRendererName(
                    renderer_[logicalScreen]
                );

            if (backend) {
                LOG_INFO(
                    "SDL",
                    "Current rendering backend for renderer " +
                    screenIndex +
                    ": " +
                    backend
                );

                const auto* formats =
                    static_cast<const SDL_PixelFormat*>(
                        SDL_GetPointerProperty(
                            SDL_GetRendererProperties(
                                renderer_[logicalScreen]
                            ),
                            SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER,
                            nullptr
                        )
                        );

                if (formats) {
                    for (;
                        *formats != SDL_PIXELFORMAT_UNKNOWN;
                        ++formats)
                    {
                        LOG_INFO(
                            "SDL",
                            "Supported pixel format: " +
                            std::string(
                                SDL_GetPixelFormatName(
                                    *formats
                                )
                            )
                        );
                    }
                }

                int interval = vSync ? 1 : 0;

                if (std::string(backend) == "opengl" &&
                    config.getProperty(
                        OPTION_GLSWAPINTERVAL,
                        interval))
                {
                    if (!SDL_SetRenderVSync(
                        renderer_[logicalScreen],
                        interval))
                    {
                        LOG_WARNING(
                            "SDL",
                            "Unable to set OpenGL renderer "
                            "swap interval: " +
                            std::string(SDL_GetError())
                        );
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------
    // Audio
    // ---------------------------------------------------------

    if (!AudioBus::instance().initialize(
        audioRate,
        audioChannels))
    {
        LOG_WARNING(
            "SDL",
            std::string(
                "Audio initialization failed: "
            ) +
            SDL_GetError()
        );
    }

    return true;
}
// Deinitialize SDL
bool SDL::deInitialize(bool fullShutdown) { // The 'fullShutdown' parameter is key
	LOG_INFO("SDL", "DeInitializing");

	// Step 1: Always destroy windows and renderers, as they are part of the video subsystem.
	if (!window_.empty() && window_[0])
	{
#ifdef __APPLE__
		SDL_SetWindowRelativeMouseMode(window_[0], false);
#endif
		SDL_WarpMouseInWindow(window_[0], windowWidth_[0] / 2, windowHeight_[0] / 2);
	}
	else
	{
		LOG_WARNING("SDL", "Window 0 is NULL, cannot center mouse within it");
	}

// Destroy render target textures
	for (auto& t : renderTargets_) {
		if (t) {
			SDL_DestroyTexture(t);
			t = nullptr;
		}
	}
	renderTargets_.clear();

	// Destroy renderers and windows
	for (auto renderer : renderer_)
	{
		if (renderer) SDL_DestroyRenderer(renderer);
	}
	renderer_.clear();

	for (auto window : window_)
	{
		if (window) SDL_DestroyWindow(window);
	}
	window_.clear();

	// Step 2: Decide which subsystems to shut down.
	if (fullShutdown)
	{
		SDL_ShowCursor();
		// This is the final application exit. Shut down everything.
		LOG_INFO("SDL", "Performing full de-initialization of all SDL subsystems.");
		AudioBus::instance().shutdown();
		SDL_Quit();

	}
	else
	{
		// This is the unloadSDL case. Shut down ONLY the video subsystem.
		LOG_INFO("SDL", "De-initializing video subsystem only.");
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}

	displayRefresh_.clear();
	screenCount_ = 0;
	numScreens_ = 0;
	numDisplays_ = 0;
	displayWidth_.clear();
	displayHeight_.clear();
	windowWidth_.clear();
	windowHeight_.clear();
	fullscreen_.clear();
	mirror_.clear();
	rotation_.clear();

	return true;
}


// Get the renderer
SDL_Renderer* SDL::getRenderer(int index) {
	if (renderer_.empty()) {
		return nullptr;
	}
	return (index >= 0 && index < screenCount_ ? renderer_[index] : renderer_[0]);
}

std::string SDL::getRendererBackend(int index) {
	SDL_Renderer* renderer = getRenderer(index);
	if (!renderer) {
		return "Invalid renderer index";
	}

	const char* name = SDL_GetRendererName(renderer);
	return name ? std::string(name) : std::string("Error getting renderer name: ") + SDL_GetError();
}

// Get the window
SDL_Window* SDL::getWindow(int index) {
	if (window_.empty()) {
		return nullptr;
	}
	return (index >= 0 && index < screenCount_ ? window_[index] : window_[0]);
}

// current target to render into for this frame
SDL_Texture* SDL::getRenderTarget(int index) {
	if (renderTargets_.empty()) return nullptr;
	return (index >= 0 && index < screenCount_ ? renderTargets_[index] : renderTargets_[0]);
}

void SDL::drawFitBars(
	int monitor,
	int layoutWidth,
	int layoutHeight) {
	// Stretch and Fill have no letterbox/pillarbox area.
	if (layoutScaleMode_ != LayoutScaleMode::Fit) {
		return;
	}

	if (monitor < 0 ||
		monitor >= screenCount_ ||
		!renderer_[monitor] ||
		layoutWidth <= 0 ||
		layoutHeight <= 0)
	{
		return;
	}

	/*
	 * Mirror mode uses its own historical scaling path in renderCopyF()
	 * and does not apply Fit/Fill scaling.
	 */
	if (mirror_[monitor]) {
		return;
	}

	SDL_Renderer* rr = renderer_[monitor];

	const int outW = windowWidth_[monitor];
	const int outH = windowHeight_[monitor];
	const int rot = rotation_[monitor] & 3;

	if (outW <= 0 || outH <= 0) {
		return;
	}

	/*
	 * Match renderCopyF():
	 *
	 * For 90/270-degree output rotation, scaling is calculated against
	 * a logical output whose width/height are swapped.
	 */
	const float logicalOutW =
		(rot & 1)
		? static_cast<float>(outH)
		: static_cast<float>(outW);

	const float logicalOutH =
		(rot & 1)
		? static_cast<float>(outW)
		: static_cast<float>(outH);

	const float scaleX =
		logicalOutW /
		static_cast<float>(layoutWidth);

	const float scaleY =
		logicalOutH /
		static_cast<float>(layoutHeight);

	const float scale =
		std::min(scaleX, scaleY);

	/*
	 * Size of the fitted virtual layout before output rotation.
	 */
	const float logicalViewportW =
		static_cast<float>(layoutWidth) * scale;

	const float logicalViewportH =
		static_cast<float>(layoutHeight) * scale;

	/*
	 * Convert fitted dimensions to final physical output orientation.
	 */
	const float viewportW =
		(rot & 1)
		? logicalViewportH
		: logicalViewportW;

	const float viewportH =
		(rot & 1)
		? logicalViewportW
		: logicalViewportH;

	/*
	 * Fit centers the virtual layout in the physical output.
	 */
	const float viewportX =
		(static_cast<float>(outW) - viewportW) * 0.5f;

	const float viewportY =
		(static_cast<float>(outH) - viewportH) * 0.5f;

	/*
	 * Round inward toward the visible viewport.
	 *
	 * This deliberately allows the black mask to cover at most a
	 * fractional edge pixel, preventing thin slivers of arbitrarily
	 * rotated geometry from leaking into the bars.
	 */
	const int left =
		std::clamp(
			static_cast<int>(std::ceil(viewportX)),
			0,
			outW
		);

	const int top =
		std::clamp(
			static_cast<int>(std::ceil(viewportY)),
			0,
			outH
		);

	const int right =
		std::clamp(
			static_cast<int>(
				std::floor(viewportX + viewportW)
				),
			0,
			outW
		);

	const int bottom =
		std::clamp(
			static_cast<int>(
				std::floor(viewportY + viewportH)
				),
			0,
			outH
		);

	SDL_FRect bars[4];
	int barCount = 0;

	// Top
	if (top > 0) {
		bars[barCount++] = {
			0,
			0,
			static_cast<float>(outW),
			static_cast<float>(top)
		};
	}

	// Bottom
	if (bottom < outH) {
		bars[barCount++] = {
			0,
			static_cast<float>(bottom),
			static_cast<float>(outW),
			static_cast<float>(outH - bottom)
		};
	}

	// Left
	if (left > 0 &&
		bottom > top)
	{
		bars[barCount++] = {
			0,
			static_cast<float>(top),
			static_cast<float>(left),
			static_cast<float>(bottom - top)
		};
	}

	// Right
	if (right < outW &&
		bottom > top)
	{
		bars[barCount++] = {
			static_cast<float>(right),
			static_cast<float>(top),
			static_cast<float>(outW - right),
			static_cast<float>(bottom - top)
		};
	}

	// Exact aspect-ratio match: nothing to mask.
	if (barCount == 0) {
		return;
	}

	SDL_SetRenderDrawColor(
		rr,
		0,
		0,
		0,
		255
	);

	/*
	 * Usually only two rectangles:
	 *
	 *   top + bottom
	 *        or
	 *   left + right
	 *
	 * One renderer call masks them all.
	 */
	SDL_RenderFillRects(
		rr,
		bars,
		barCount
	);
}

// Render a copy of a texture
bool SDL::renderCopy(SDL_Texture* texture, float alpha, const SDL_Rect* src, const SDL_Rect* dest,
	ViewInfo& viewInfo, int layoutWidth, int layoutHeight) {
	if (!dest) return false;
	const SDL_FRect destination{ static_cast<float>(dest->x), static_cast<float>(dest->y),
		static_cast<float>(dest->w), static_cast<float>(dest->h) };
	return renderCopyF(texture, alpha, src, &destination, viewInfo, layoutWidth, layoutHeight);
}

bool SDL::renderCopyF(SDL_Texture* texture,
    float alpha,
    const SDL_Rect* src,
    const SDL_FRect* dest,
    ViewInfo& viewInfo,
    int layoutWidth,
    int layoutHeight) {
    return renderCopyFImpl(nullptr, texture, alpha, src, dest, viewInfo, layoutWidth, layoutHeight);
}

bool SDL::appendCopyF(GeometryBatch& batch, SDL_Texture* texture, float alpha,
    const SDL_Rect* src, const SDL_FRect* dest, ViewInfo& viewInfo,
    int layoutWidth, int layoutHeight) {
    return renderCopyFImpl(&batch, texture, alpha, src, dest, viewInfo, layoutWidth, layoutHeight);
}

bool SDL::renderCopyFImpl(GeometryBatch* batch, SDL_Texture* texture, float alpha,
    const SDL_Rect* src, const SDL_FRect* dest, ViewInfo& viewInfo,
    int layoutWidth, int layoutHeight) {
    if (!texture) {
        return false;
    }

    if (alpha <= 0.0f) {
        return true;
    }

    if (!dest || layoutWidth <= 0 || layoutHeight <= 0) {
        return false;
    }

    const int m = viewInfo.Monitor;

    if (m < 0 || m >= screenCount_ || !renderer_[m]) {
        return true;
    }

    // ---------------------------------------------------------------------
    // Cached layout -> output transform.
    // ---------------------------------------------------------------------

    struct ScaleCache {
        int lastLW = -1;
        int lastLH = -1;
        int lastOW = -1;
        int lastOH = -1;
        int lastRot = -1;
        int lastMode = -1;
        int lastDisplayW = -1;
        int lastDisplayH = -1;

        bool lastMir = false;
        bool lastFS = false;

        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;

        // Fullscreen-window offsets remain in layout coordinates.
        float dxL = 0.0f;
        float dyL = 0.0f;
    };

    static std::vector<ScaleCache> cache;

    // Do not call resize() unconditionally on every rendered component.
    if (cache.size() < static_cast<size_t>(screenCount_)) {
        cache.resize(static_cast<size_t>(screenCount_));
    }

    const int outW = windowWidth_[m];
    const int outH = windowHeight_[m];
    const int rot = rotation_[m] & 3;
    const bool mir = mirror_[m];
    const bool fs = fullscreen_[m];
    const int modeKey = static_cast<int>(layoutScaleMode_);

    if (outW <= 0 || outH <= 0) {
        return true;
    }

    ScaleCache& sc = cache[m];

    const bool cacheInvalid =
        sc.lastDisplayW != displayWidth_[m] ||
        sc.lastDisplayH != displayHeight_[m] ||
        sc.lastLW != layoutWidth ||
        sc.lastLH != layoutHeight ||
        sc.lastOW != outW ||
        sc.lastOH != outH ||
        sc.lastRot != rot ||
        sc.lastMir != mir ||
        sc.lastFS != fs ||
        sc.lastMode != modeKey;

    if (cacheInvalid) {
        const float logicalOutW =
            (rot & 1)
            ? static_cast<float>(outH)
            : static_cast<float>(outW);

        const float logicalOutH =
            (rot & 1)
            ? static_cast<float>(outW)
            : static_cast<float>(outH);

        const float candidateScaleX =
            logicalOutW / static_cast<float>(layoutWidth);

        const float candidateScaleY =
            logicalOutH / static_cast<float>(layoutHeight);

        sc.offsetX = 0.0f;
        sc.offsetY = 0.0f;

        if (mir) {
            // Preserve existing mirror behavior.
            sc.scaleX = candidateScaleX;
            sc.scaleY = candidateScaleY * 0.5f;
        }
        else {
            switch (layoutScaleMode_) {
                case LayoutScaleMode::Fit: {
                    const float scale =
                        std::min(candidateScaleX, candidateScaleY);

                    sc.scaleX = scale;
                    sc.scaleY = scale;

                    sc.offsetX =
                        (logicalOutW -
                            static_cast<float>(layoutWidth) * scale) *
                        0.5f;

                    sc.offsetY =
                        (logicalOutH -
                            static_cast<float>(layoutHeight) * scale) *
                        0.5f;

                    break;
                }

                case LayoutScaleMode::Fill: {
                    const float scale =
                        std::max(candidateScaleX, candidateScaleY);

                    sc.scaleX = scale;
                    sc.scaleY = scale;

                    sc.offsetX =
                        (logicalOutW -
                            static_cast<float>(layoutWidth) * scale) *
                        0.5f;

                    sc.offsetY =
                        (logicalOutH -
                            static_cast<float>(layoutHeight) * scale) *
                        0.5f;

                    break;
                }

                case LayoutScaleMode::Stretch:
                default:
                sc.scaleX = candidateScaleX;
                sc.scaleY = candidateScaleY;
                break;
            }
        }

        sc.dxL = 0.0f;
        sc.dyL = 0.0f;

        if (fs) {
            sc.dxL =
                0.5f *
                static_cast<float>(displayWidth_[m] - outW) /
                std::max(sc.scaleX, 1e-6f);

            sc.dyL =
                0.5f *
                static_cast<float>(displayHeight_[m] - outH) /
                std::max(sc.scaleY, 1e-6f);
        }

        sc.lastDisplayW = displayWidth_[m];
        sc.lastDisplayH = displayHeight_[m];
        sc.lastLW = layoutWidth;
        sc.lastLH = layoutHeight;
        sc.lastOW = outW;
        sc.lastOH = outH;
        sc.lastRot = rot;
        sc.lastMir = mir;
        sc.lastFS = fs;
        sc.lastMode = modeKey;
    }

    const float scaleX = sc.scaleX;
    const float scaleY = sc.scaleY;

    // ---------------------------------------------------------------------
    // Texture/source setup.
    //
    // SDL3 exposes w/h as public read-only fields on SDL_Texture.
    // Avoid SDL_GetTextureSize() in this hot path.
    // ---------------------------------------------------------------------

    const float texW = static_cast<float>(texture->w);
    const float texH = static_cast<float>(texture->h);

    if (texW <= 0.0f || texH <= 0.0f) {
        return false;
    }

    if (viewInfo.ImageWidth <= 0.0f) {
        viewInfo.ImageWidth = texW;
    }

    if (viewInfo.ImageHeight <= 0.0f) {
        viewInfo.ImageHeight = texH;
    }

    const float invTexW = 1.0f / texW;
    const float invTexH = 1.0f / texH;

    // SDL_RenderGeometry ignores SDL's texture modulation state, so preserve
    // RetroFE's existing RGB behavior explicitly in the vertex colors.
    float textureR = 1.0f;
    float textureG = 1.0f;
    float textureB = 1.0f;

    SDL_GetTextureColorModFloat(
        texture,
        &textureR,
        &textureG,
        &textureB);

    SDL_FRect srcRect =
        src
        ? SDL_FRect{
            static_cast<float>(src->x),
            static_cast<float>(src->y),
            static_cast<float>(src->w),
            static_cast<float>(src->h) }
    : SDL_FRect{ 0.0f, 0.0f, texW, texH };

    SDL_FRect dstRect = *dest;

    dstRect.x += sc.dxL;
    dstRect.y += sc.dyL;

    if (dstRect.w <= 0.0f ||
        dstRect.h <= 0.0f ||
        srcRect.w <= 0.0f ||
        srcRect.h <= 0.0f)
    {
        return true;
    }

    // ---------------------------------------------------------------------
    // Container clipping.
    // ---------------------------------------------------------------------

    SDL_FRect container{};

    bool hasContainer =
        viewInfo.ContainerWidth > 0.0f &&
        viewInfo.ContainerHeight > 0.0f;

    if (mir && !hasContainer) {
        container = {
            0.0f,
            0.0f,
            static_cast<float>(layoutWidth),
            static_cast<float>(layoutHeight)
        };

        hasContainer = true;
    }
    else if (hasContainer) {
        container = {
            viewInfo.ContainerX,
            viewInfo.ContainerY,
            viewInfo.ContainerWidth,
            viewInfo.ContainerHeight
        };
    }

    const SDL_FRect src0 = srcRect;
    const SDL_FRect dst0 = dstRect;

    // ---------------------------------------------------------------------
    // Transform helpers.
    // ---------------------------------------------------------------------

    auto to_pixels =
        [&](SDL_FRect rect) -> SDL_FRect
        {
            rect.x = sc.offsetX + rect.x * scaleX;
            rect.y = sc.offsetY + rect.y * scaleY;
            rect.w *= scaleX;
            rect.h *= scaleY;
            return rect;
        };

    auto clip_to_rect =
        [&](SDL_FRect& sourceRect,
            SDL_FRect& destinationRect,
            const SDL_FRect& clipRect)
        {
            if (destinationRect.w <= 0.0f ||
                destinationRect.h <= 0.0f ||
                clipRect.w <= 0.0f ||
                clipRect.h <= 0.0f)
            {
                destinationRect.w = 0.0f;
                destinationRect.h = 0.0f;
                sourceRect.w = 0.0f;
                sourceRect.h = 0.0f;
                return;
            }

            const float clipRight =
                clipRect.x + clipRect.w;

            const float clipBottom =
                clipRect.y + clipRect.h;

            const float destinationRight =
                destinationRect.x + destinationRect.w;

            const float destinationBottom =
                destinationRect.y + destinationRect.h;

            // Entirely outside.
            if (destinationRight <= clipRect.x ||
                destinationBottom <= clipRect.y ||
                destinationRect.x >= clipRight ||
                destinationRect.y >= clipBottom)
            {
                destinationRect.w = 0.0f;
                destinationRect.h = 0.0f;
                sourceRect.w = 0.0f;
                sourceRect.h = 0.0f;
                return;
            }

            // Common case: already entirely contained.
            if (destinationRect.x >= clipRect.x &&
                destinationRect.y >= clipRect.y &&
                destinationRight <= clipRight &&
                destinationBottom <= clipBottom)
            {
                return;
            }

            const SDL_FRect sourceCopy = sourceRect;
            const SDL_FRect destinationCopy = destinationRect;

            const float clippedRight =
                std::min(destinationRight, clipRight);

            const float clippedBottom =
                std::min(destinationBottom, clipBottom);

            destinationRect.x =
                std::max(destinationCopy.x, clipRect.x);

            destinationRect.y =
                std::max(destinationCopy.y, clipRect.y);

            destinationRect.w =
                clippedRight - destinationRect.x;

            destinationRect.h =
                clippedBottom - destinationRect.y;

            const float sourceScaleX =
                destinationCopy.w > 0.0f
                ? sourceCopy.w / destinationCopy.w
                : 0.0f;

            const float sourceScaleY =
                destinationCopy.h > 0.0f
                ? sourceCopy.h / destinationCopy.h
                : 0.0f;

            sourceRect.x =
                sourceCopy.x +
                (destinationRect.x - destinationCopy.x) *
                sourceScaleX;

            sourceRect.y =
                sourceCopy.y +
                (destinationRect.y - destinationCopy.y) *
                sourceScaleY;

            sourceRect.w =
                destinationRect.w * sourceScaleX;

            sourceRect.h =
                destinationRect.h * sourceScaleY;

            const SDL_FRect limit =
                src
                ? SDL_FRect{
                    static_cast<float>(src->x),
                    static_cast<float>(src->y),
                    static_cast<float>(src->w),
                    static_cast<float>(src->h) }
                    : SDL_FRect{
                        0.0f,
                        0.0f,
                        texW,
                        texH };

            if (sourceRect.x < limit.x) {
                sourceRect.x = limit.x;
            }

            if (sourceRect.y < limit.y) {
                sourceRect.y = limit.y;
            }

            if (sourceRect.x + sourceRect.w >
                limit.x + limit.w)
            {
                sourceRect.w =
                    std::max(
                        0.0f,
                        limit.x + limit.w - sourceRect.x);
            }

            if (sourceRect.y + sourceRect.h >
                limit.y + limit.h)
            {
                sourceRect.h =
                    std::max(
                        0.0f,
                        limit.y + limit.h - sourceRect.y);
            }
        };

    auto apply_output_rotation_rect =
        [&](SDL_FRect& rectPixels)
        {
            switch (rot) {
                case 1: {
                    const float oldX = rectPixels.x;

                    rectPixels.x =
                        static_cast<float>(outW) -
                        rectPixels.y -
                        rectPixels.h * 0.5f -
                        rectPixels.w * 0.5f;

                    rectPixels.y =
                        oldX -
                        rectPixels.h * 0.5f +
                        rectPixels.w * 0.5f;

                    break;
                }

                case 2:
                rectPixels.x =
                    static_cast<float>(outW) -
                    rectPixels.x -
                    rectPixels.w;

                rectPixels.y =
                    static_cast<float>(outH) -
                    rectPixels.y -
                    rectPixels.h;

                break;

                case 3: {
                    const float oldX = rectPixels.x;

                    rectPixels.x =
                        rectPixels.y +
                        rectPixels.h * 0.5f -
                        rectPixels.w * 0.5f;

                    rectPixels.y =
                        static_cast<float>(outH) -
                        oldX -
                        rectPixels.h * 0.5f -
                        rectPixels.w * 0.5f;

                    break;
                }

                default:
                break;
            }
        };

    // ---------------------------------------------------------------------
    // Geometry accumulator.
    //
    // Maximum:
    //   base + four reflection directions = 5 paths
    //   mirror mode emits two quads per path
    //
    //   10 quads
    //   40 vertices
    //   60 indices
    //
    // Fixed stack storage avoids allocation.
    // ---------------------------------------------------------------------

    constexpr int MAX_QUADS = 10;
    constexpr int VERTS_PER_QUAD = 4;
    constexpr int INDICES_PER_QUAD = 6;

    std::array<SDL_Vertex, MAX_QUADS* VERTS_PER_QUAD> vertices;
    std::array<int, MAX_QUADS* INDICES_PER_QUAD> indices;

    int vertexCount = 0;
    int indexCount = 0;

    auto emit_quad =
        [&](const SDL_FRect& sourceRect,
            const SDL_FRect& destinationPixels,
            float angleDegrees,
            bool flipHorizontal,
            bool flipVertical,
            float alpha01) -> bool
        {
            constexpr float epsilon = 1.0f;

            if (vertexCount + VERTS_PER_QUAD >
                static_cast<int>(vertices.size()) ||
                indexCount + INDICES_PER_QUAD >
                static_cast<int>(indices.size()))
            {
                return false;
            }

            SDL_FPoint points[4];

            // -------------------------------------------------------------
            // Common fast path: no component/output rotation.
            // -------------------------------------------------------------

            if (angleDegrees == 0.0f) {
                if (destinationPixels.x + destinationPixels.w < -epsilon ||
                    destinationPixels.y + destinationPixels.h < -epsilon ||
                    destinationPixels.x >
                    static_cast<float>(outW) + epsilon ||
                    destinationPixels.y >
                    static_cast<float>(outH) + epsilon)
                {
                    return true;
                }

                points[0] = {
                    destinationPixels.x,
                    destinationPixels.y
                };

                points[1] = {
                    destinationPixels.x + destinationPixels.w,
                    destinationPixels.y
                };

                points[2] = {
                    destinationPixels.x + destinationPixels.w,
                    destinationPixels.y + destinationPixels.h
                };

                points[3] = {
                    destinationPixels.x,
                    destinationPixels.y + destinationPixels.h
                };
            }
            else {
                float cosAngle;
                float sinAngle;

                const float remainder =
                    std::fmod(std::fabs(angleDegrees), 90.0f);

                // Avoid trig for exact output quarter-turns.
                if (remainder < 0.001f ||
                    remainder > 89.999f)
                {
                    int quarter =
                        static_cast<int>(
                            std::lround(angleDegrees / 90.0f)) % 4;

                    if (quarter < 0) {
                        quarter += 4;
                    }

                    static constexpr float cosTable[4] = {
                        1.0f, 0.0f, -1.0f, 0.0f
                    };

                    static constexpr float sinTable[4] = {
                        0.0f, 1.0f, 0.0f, -1.0f
                    };

                    cosAngle = cosTable[quarter];
                    sinAngle = sinTable[quarter];
                }
                else {
                    const float radians =
                        angleDegrees *
                        (3.14159265358979323846f / 180.0f);

                    cosAngle = std::cos(radians);
                    sinAngle = std::sin(radians);
                }

                const float centerX =
                    destinationPixels.x +
                    0.5f * destinationPixels.w;

                const float centerY =
                    destinationPixels.y +
                    0.5f * destinationPixels.h;

                const float halfExtentX =
                    0.5f *
                    (std::fabs(cosAngle) *
                        destinationPixels.w +
                        std::fabs(sinAngle) *
                        destinationPixels.h);

                const float halfExtentY =
                    0.5f *
                    (std::fabs(sinAngle) *
                        destinationPixels.w +
                        std::fabs(cosAngle) *
                        destinationPixels.h);

                if (centerX + halfExtentX < -epsilon ||
                    centerY + halfExtentY < -epsilon ||
                    centerX - halfExtentX >
                    static_cast<float>(outW) + epsilon ||
                    centerY - halfExtentY >
                    static_cast<float>(outH) + epsilon)
                {
                    return true;
                }

                points[0] = {
                    destinationPixels.x,
                    destinationPixels.y
                };

                points[1] = {
                    destinationPixels.x + destinationPixels.w,
                    destinationPixels.y
                };

                points[2] = {
                    destinationPixels.x + destinationPixels.w,
                    destinationPixels.y + destinationPixels.h
                };

                points[3] = {
                    destinationPixels.x,
                    destinationPixels.y + destinationPixels.h
                };

                for (SDL_FPoint& point : points) {
                    const float translatedX =
                        point.x - centerX;

                    const float translatedY =
                        point.y - centerY;

                    point.x =
                        translatedX * cosAngle -
                        translatedY * sinAngle +
                        centerX;

                    point.y =
                        translatedX * sinAngle +
                        translatedY * cosAngle +
                        centerY;
                }
            }

            float u0 =
                sourceRect.x * invTexW;

            float v0 =
                sourceRect.y * invTexH;

            float u1 =
                (sourceRect.x + sourceRect.w) * invTexW;

            float v1 =
                (sourceRect.y + sourceRect.h) * invTexH;

            if (flipHorizontal) {
                std::swap(u0, u1);
            }

            if (flipVertical) {
                std::swap(v0, v1);
            }

            const SDL_FColor color = {
                textureR,
                textureG,
                textureB,
                std::clamp(alpha01, 0.0f, 1.0f)
            };

            const int base = vertexCount;

            vertices[vertexCount++] = {
                points[0],
                color,
                {u0, v0}
            };

            vertices[vertexCount++] = {
                points[1],
                color,
                {u1, v0}
            };

            vertices[vertexCount++] = {
                points[2],
                color,
                {u1, v1}
            };

            vertices[vertexCount++] = {
                points[3],
                color,
                {u0, v1}
            };

            indices[indexCount++] = base + 0;
            indices[indexCount++] = base + 1;
            indices[indexCount++] = base + 2;

            indices[indexCount++] = base + 0;
            indices[indexCount++] = base + 2;
            indices[indexCount++] = base + 3;

            return true;
        };

    // ---------------------------------------------------------------------
    // Generate one logical rendering path.
    // ---------------------------------------------------------------------

    auto render_path =
        [&](bool reflect, int kind = -1) -> bool
        {
            // Very cheap layout-space rejection for the common,
            // non-reflected, unrotated element.
            if (!reflect &&
                viewInfo.Angle == 0.0f)
            {
                if (dst0.x + dst0.w <= 0.0f ||
                    dst0.y + dst0.h <= 0.0f ||
                    dst0.x >= static_cast<float>(layoutWidth) ||
                    dst0.y >= static_cast<float>(layoutHeight))
                {
                    return true;
                }
            }

            SDL_FRect sourceRect = src0;
            SDL_FRect destinationRect = dst0;

            float pathAlpha = alpha;
            bool flipHorizontal = false;
            bool flipVertical = false;

            if (reflect) {
                pathAlpha =
                    alpha * viewInfo.ReflectionAlpha;

                flipHorizontal =
                    kind == 2 || kind == 3;

                flipVertical =
                    kind == 0 || kind == 1;

                switch (kind) {
                    case 0:
                    destinationRect.h *=
                        viewInfo.ReflectionScale;

                    destinationRect.y -=
                        destinationRect.h +
                        viewInfo.ReflectionDistance;

                    break;

                    case 1:
                    destinationRect.y +=
                        destinationRect.h +
                        viewInfo.ReflectionDistance;

                    destinationRect.h *=
                        viewInfo.ReflectionScale;

                    break;

                    case 2:
                    destinationRect.w *=
                        viewInfo.ReflectionScale;

                    destinationRect.x -=
                        destinationRect.w +
                        viewInfo.ReflectionDistance;

                    break;

                    case 3:
                    destinationRect.x +=
                        destinationRect.w +
                        viewInfo.ReflectionDistance;

                    destinationRect.w *=
                        viewInfo.ReflectionScale;

                    break;

                    default:
                    break;
                }
            }

            if (hasContainer) {
                clip_to_rect(
                    sourceRect,
                    destinationRect,
                    container);
            }

            if (destinationRect.w <= 0.0f ||
                destinationRect.h <= 0.0f ||
                sourceRect.w <= 0.0f ||
                sourceRect.h <= 0.0f)
            {
                return true;
            }

            float angle = viewInfo.Angle;

            if (!mir) {
                angle +=
                    static_cast<float>(rot * 90);
            }

            SDL_FRect destinationPixels =
                to_pixels(destinationRect);

            bool result = true;

            if (mir) {
                if ((rotation_[m] & 1) == 0) {
                    SDL_FRect mirroredRect =
                        destinationPixels;

                    mirroredRect.y +=
                        static_cast<float>(outH) *
                        0.5f;

                    result &=
                        emit_quad(
                            sourceRect,
                            mirroredRect,
                            angle,
                            flipHorizontal,
                            flipVertical,
                            pathAlpha);

                    mirroredRect.x =
                        static_cast<float>(outW) -
                        mirroredRect.x -
                        mirroredRect.w;

                    mirroredRect.y =
                        static_cast<float>(outH) -
                        mirroredRect.y -
                        mirroredRect.h;

                    result &=
                        emit_quad(
                            sourceRect,
                            mirroredRect,
                            angle + 180.0f,
                            flipHorizontal,
                            flipVertical,
                            pathAlpha);
                }
                else {
                    SDL_FRect mirroredRect =
                        destinationPixels;

                    const float oldX =
                        mirroredRect.x;

                    mirroredRect.x =
                        static_cast<float>(outW) * 0.5f -
                        mirroredRect.y -
                        mirroredRect.h * 0.5f -
                        mirroredRect.w * 0.5f;

                    mirroredRect.y =
                        oldX -
                        mirroredRect.h * 0.5f +
                        mirroredRect.w * 0.5f;

                    result &=
                        emit_quad(
                            sourceRect,
                            mirroredRect,
                            angle + 90.0f,
                            flipHorizontal,
                            flipVertical,
                            pathAlpha);

                    mirroredRect.x =
                        static_cast<float>(outW) -
                        mirroredRect.x -
                        mirroredRect.w;

                    mirroredRect.y =
                        static_cast<float>(outH) -
                        mirroredRect.y -
                        mirroredRect.h;

                    result &=
                        emit_quad(
                            sourceRect,
                            mirroredRect,
                            angle + 270.0f,
                            flipHorizontal,
                            flipVertical,
                            pathAlpha);
                }
            }
            else {
                apply_output_rotation_rect(
                    destinationPixels);

                result &=
                    emit_quad(
                        sourceRect,
                        destinationPixels,
                        angle,
                        flipHorizontal,
                        flipVertical,
                        pathAlpha);
            }

            return result;
        };

    // ---------------------------------------------------------------------
    // Build all geometry first.
    // ---------------------------------------------------------------------

    bool result =
        render_path(false);

    if (viewInfo.hasReflection) {
        for (int reflectionKind = 0;
            reflectionKind < 4;
            ++reflectionKind)
        {
            if (viewInfo.reflectionMask &
                (1 << reflectionKind))
            {
                result &=
                    render_path(
                        true,
                        reflectionKind);
            }
        }
    }

    if (!result) {
        return false;
    }

    // Everything was culled.
    if (vertexCount == 0) {
        return true;
    }

    // One SDL command for base image + reflections + mirror copies.
    if (batch) {
        return batch->append(renderer_[m], texture, vertices.data(), vertexCount,
            indices.data(), indexCount);
    }
    return SDL_RenderGeometry(
        renderer_[m],
        texture,
        vertices.data(),
        vertexCount,
        indices.data(),
        indexCount);
}

bool SDL::beginVideoFrame(SDL_Renderer* renderer) {
#ifdef RETROFE_HAVE_D3D12
    return D3D12VideoInterop::beginFrame(renderer);
#else
    (void)renderer;
    return true;
#endif
}
