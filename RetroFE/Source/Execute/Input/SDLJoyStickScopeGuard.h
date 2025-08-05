// Source/Execute/Input/SDLJoystickScopeGuard.h

#pragma once

#include <vector>
#include <string>

// Assuming SDL headers are available via the build system's include paths
#include <SDL3/SDL.h>

// Assuming your logging utility is accessible from this location.
// Adjust the path if necessary based on your project structure.
#include "../../Utility/Log.h"

/**
 * @brief An RAII scope guard to manage a temporary SDL Joystick session.
 *
 * This class checks if the SDL_INIT_JOYSTICK subsystem is already active.
 * If not, it initializes it upon construction and automatically de-initializes it
 * upon destruction. If the subsystem was already running, this class does nothing,
 * ensuring it doesn't interfere with a pre-existing SDL session.
 */
struct SDLJoystickScopeGuard {
	bool initialized_by_me = false;
	std::vector<SDL_Joystick*> joysticks;

	SDLJoystickScopeGuard() {
		// Check if the joystick subsystem is already running.
		if (SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD) != 0) {
			LOG_INFO("Launcher", "Using existing SDL joystick session for input monitoring.");
			// We don't need to do anything else. The main RetroFE instance is handling it.
			// Joysticks are assumed to be open already.
		}
		else {
			// SDL is not initialized, so we must do it.
			LOG_INFO("Launcher", "SDL joystick session not found. Initializing a temporary one.");
			if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD) == 0) {
				initialized_by_me = true; // We are responsible for cleanup.
				SDL_SetJoystickEventsEnabled(true);

				// Enumerate and open all joysticks
				int numJoysticks;
				SDL_JoystickID* joystickIDs = SDL_GetJoysticks(&numJoysticks);
				for (int i = 0; i < numJoysticks; ++i) {
					SDL_Joystick* joy = SDL_OpenJoystick(joystickIDs[i]);
					if (joy) {
						joysticks.push_back(joy);
					}
				}
				SDL_free(joystickIDs);
				LOG_INFO("Launcher", "Temporary SDL joystick subsystem initialized successfully.");
			}
			else {
				LOG_ERROR("Launcher", "Failed to init temporary SDL joystick subsystem for launcher.");
			}
		}
	}

	~SDLJoystickScopeGuard() {
		// Only shut down the subsystem if we were the one who started it.
		if (initialized_by_me) {
			for (auto joy : joysticks) {
				if (joy) SDL_CloseJoystick(joy);
			}
			joysticks.clear();
			SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
			LOG_INFO("Launcher", "Temporary SDL joystick subsystem deinitialized.");
		}
		// If initialized_by_me is false, we do nothing and leave the main session alone.
	}

	// Disable copying and assignment to prevent incorrect resource management.
	SDLJoystickScopeGuard(const SDLJoystickScopeGuard&) = delete;
	SDLJoystickScopeGuard& operator=(const SDLJoystickScopeGuard&) = delete;
};