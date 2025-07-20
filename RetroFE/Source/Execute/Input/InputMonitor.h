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
    /**
     * @brief Constructs an InputMonitor and configures it from settings.
     * @param config The global configuration object to read settings from.
     */
    explicit InputMonitor(Configuration& config) {
        // Load the quit combo configuration
        std::vector<std::string> quitCombo = { "joyButton6", "joyButton7" }; // Default: BACK+START
        std::string quitComboStr;
        if (config.getProperty("controls.quitCombo", quitComboStr)) {
            quitCombo.clear(); // Clear default
            Utils::listToVector(quitComboStr, quitCombo, ',');
        }

        // Parse the string representation into integer indices for faster checking
        for (const auto& btn : quitCombo) {
            if (btn.rfind("joyButton", 0) == 0) {
                try {
                    int idx = std::stoi(btn.substr(9));
                    quitComboIndices_.push_back(idx);
                }
                catch (const std::exception&) {
                    LOG_ERROR("InputMonitor", "Failed to parse quit combo button: " + btn);
                }
            }
        }
    }

    /**
     * @brief Polls SDL for events and determines if a significant input occurred.
     * @return An InputDetectionResult indicating what was found.
     */
    InputDetectionResult checkSdlEvents();

    /**
     * @brief Checks if the very first input detected was the quit combo.
     *
     * This is useful for attract mode logic to differentiate between a user
     * wanting to quit immediately vs. wanting to play the game.
     * @return True if the first input was the quit combo, false otherwise.
     */
    bool wasQuitFirstInput() const {
        return firstInputWasQuit_;
    }

    /**
     * @brief Resets the internal state of the monitor.
     *
     * Should be called before starting to monitor a new process launch.
     */
    void reset() {
        joystickButtonState_.clear();
        joystickButtonTimeState_.clear();
        anyInputRegistered_ = false;
        firstInputWasQuit_ = false;
    }

    // This class manages state and an RAII resource, so it should not be copied.
    InputMonitor(const InputMonitor&) = delete;
    InputMonitor& operator=(const InputMonitor&) = delete;

private:
    // The RAII guard that ensures the SDL joystick subsystem is running.
    // This is the FIRST member initialized and the LAST one destroyed.
    SDLJoystickScopeGuard sdlGuard_;

    // --- Configuration State ---
    std::vector<int> quitComboIndices_;

    // --- Dynamic Input State ---
    std::map<SDL_JoystickID, std::map<int, bool>> joystickButtonState_;
    std::map<SDL_JoystickID, std::map<int, std::chrono::high_resolution_clock::time_point>> joystickButtonTimeState_;

    // --- High-Level Logic State ---
    bool anyInputRegistered_ = false;
    bool firstInputWasQuit_ = false;
};