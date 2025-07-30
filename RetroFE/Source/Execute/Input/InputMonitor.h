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

#pragma once

#include <vector>
#include <string>
#include <map>
#include <set> 
#include <chrono>

#include <SDL2/SDL.h>

#include "SDLJoyStickScopeGuard.h"
#include "../../Database/Configuration.h" // For reading config values
#include "../../Utility/Utils.h"         // For listToVector
#include "../../Utility/Log.h"

/**
 * @brief Represents the type of input detected during a poll.
 */
enum class InputDetectionResult {
    NoInput,        // No relevant input was detected.
    PlayInput,      // A generic input (not the quit combo) was detected.
    QuitInput       // The specific quit combo was detected.
};

/**
 * @brief Manages and monitors user input during game execution.
 *
 * This class encapsulates the logic for detecting a specific "quit combo"
 * from joystick input. It manages its own temporary SDL joystick session
 * and keeps track of input state to differentiate between a user intending
 * to quit versus a user just starting to play.
 */
class InputMonitor {
public:
    // Constructor is now just a declaration. Its implementation will move to the .cpp file.
    explicit InputMonitor(Configuration& config);

    InputDetectionResult checkSdlEvents();

    bool wasQuitFirstInput() const {
        return firstInputWasQuit_;
    }

    void reset() {
        joystickButtonState_.clear();
        joystickButtonTimeState_.clear();
        anyInputRegistered_ = false;
        firstInputWasQuit_ = false;
    }

    InputMonitor(const InputMonitor&) = delete;
    InputMonitor& operator=(const InputMonitor&) = delete;

private:

    // --- UPDATED Configuration State ---
    std::set<int> singleQuitButtonIndices_; // <-- NEW: For `quit = joyButton0`
    std::vector<int> quitComboIndices_;     // <-- EXISTING: For `controls.quitCombo = ...`

    // --- Dynamic Input State (Unchanged) ---
    std::map<SDL_JoystickID, std::map<int, bool>> joystickButtonState_;
    std::map<SDL_JoystickID, std::map<int, std::chrono::high_resolution_clock::time_point>> joystickButtonTimeState_;

    // --- High-Level Logic State (Unchanged) ---
    bool anyInputRegistered_ = false;
    bool firstInputWasQuit_ = false;
};