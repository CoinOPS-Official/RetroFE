#pragma once

#include <map>
#include <SDL3/SDL.h>
#include "../../Utility/Log.h"

// Construct, update and destroy on the SDL main thread. Each subsystem/open
// reference is balanced even when borrowing an already initialized SDL session.
struct SDLJoystickScopeGuard {
    bool initialized_by_me = false;
    bool gamepadMode;
    std::map<SDL_JoystickID, SDL_Joystick*> joysticks;
    std::map<SDL_JoystickID, SDL_Gamepad*> gamepads;

    explicit SDLJoystickScopeGuard(bool useGamepads = false) : gamepadMode(useGamepads) {
        if (!SDL_IsMainThread()) {
            LOG_ERROR("Launcher", "SDL input monitoring must start on the main thread.");
            return;
        }
        if (!SDL_InitSubSystem(flags())) {
            LOG_ERROR("Launcher", "Failed to initialize SDL input monitoring: " << SDL_GetError());
            return;
        }
        initialized_by_me = true;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        if (!ids) {
            LOG_ERROR("Launcher", "Failed to enumerate joysticks: " << SDL_GetError());
        } else {
            for (int i = 0; i < count; ++i) open(ids[i]);
            SDL_free(ids);
        }
    }

    void open(SDL_JoystickID id) {
        if (!initialized_by_me || !id || joysticks.count(id) || gamepads.count(id)) return;
        if (gamepadMode && SDL_IsGamepad(id)) {
            if (auto* pad = SDL_OpenGamepad(id)) gamepads.emplace(id, pad);
            else LOG_ERROR("Launcher", "Failed to open gamepad: " << SDL_GetError());
        } else {
            if (auto* joy = SDL_OpenJoystick(id)) joysticks.emplace(id, joy);
            else LOG_ERROR("Launcher", "Failed to open joystick: " << SDL_GetError());
        }
    }

    void remove(SDL_JoystickID id) {
        auto pad = gamepads.find(id);
        if (pad != gamepads.end()) {
            SDL_CloseGamepad(pad->second);
            gamepads.erase(pad);
        }
        auto joy = joysticks.find(id);
        if (joy != joysticks.end()) {
            SDL_CloseJoystick(joy->second);
            joysticks.erase(joy);
        }
    }

    ~SDLJoystickScopeGuard() {
        if (!initialized_by_me) return;
        for (auto& pad : gamepads) SDL_CloseGamepad(pad.second);
        for (auto& joy : joysticks) SDL_CloseJoystick(joy.second);
        SDL_QuitSubSystem(flags());
    }

    SDL_InitFlags flags() const {
        return gamepadMode ? SDL_INIT_GAMEPAD : SDL_INIT_JOYSTICK;
    }

    SDLJoystickScopeGuard(const SDLJoystickScopeGuard&) = delete;
    SDLJoystickScopeGuard& operator=(const SDLJoystickScopeGuard&) = delete;
};