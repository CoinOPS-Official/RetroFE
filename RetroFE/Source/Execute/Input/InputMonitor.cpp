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

#include "InputMonitor.h"

InputMonitor::InputMonitor(Configuration& config) {
    // Read the "quit" binding for single joystick buttons.
    std::string quitBtnsStr;
    if (config.getProperty("quit", quitBtnsStr)) {
        std::vector<std::string> signals;
        Utils::listToVector(quitBtnsStr, signals, ',');
        for (const auto& signal : signals) {
            // We only care about joystick buttons for background monitoring.
            if (signal.rfind("joyButton", 0) == 0) {
                try {
                    int idx = std::stoi(signal.substr(9));
                    singleQuitButtonIndices_.insert(idx);
                    LOG_DEBUG("InputMonitor", "Registered single quit button index: " + std::to_string(idx));
                }
                catch (const std::exception& e) {
                    LOG_ERROR("InputMonitor", "Failed to parse single quit button: " + signal + " (" + e.what() + ")");
                }
            }
        }
    }

    // Read the "quitCombo" binding for multi-button joystick combos.
    std::string quitComboStr;
    if (config.getProperty("controls.quitCombo", quitComboStr)) {
        std::vector<std::string> signals;
        Utils::listToVector(quitComboStr, signals, ',');
        for (const auto& signal : signals) {
            if (signal.rfind("joyButton", 0) == 0) {
                try {
                    int idx = std::stoi(signal.substr(9));
                    quitComboIndices_.push_back(idx);
                    LOG_DEBUG("InputMonitor", "Registered combo quit button index: " + std::to_string(idx));
                }
                catch (const std::exception& e) {
                    LOG_ERROR("InputMonitor", "Failed to parse combo quit button: " + signal + " (" + e.what() + ")");
                }
            }
        }
    }
}

InputDetectionResult InputMonitor::checkSdlEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_JOYBUTTONDOWN) {
            int buttonIdx = e.jbutton.button;

            if (singleQuitButtonIndices_.count(buttonIdx) > 0) {
                // This is a dedicated single-button quit.
                if (!anyInputRegistered_) {
                    firstInputWasQuit_ = true;
                    LOG_INFO("InputMonitor", "Single quit button " + std::to_string(buttonIdx) + " detected (first input).");
                }
                else {
                    LOG_INFO("InputMonitor", "Single quit button " + std::to_string(buttonIdx) + " detected.");
                }
                anyInputRegistered_ = true;
                return InputDetectionResult::QuitInput;
            }

            joystickButtonState_[e.jbutton.which][buttonIdx] = true;
            joystickButtonTimeState_[e.jbutton.which][buttonIdx] = std::chrono::high_resolution_clock::now();

            bool isQuitCombo = true;
            if (quitComboIndices_.empty()) {
                isQuitCombo = false;
            }
            else {
                for (int idx : quitComboIndices_) {
                    if (joystickButtonState_[e.jbutton.which].count(idx) == 0 || !joystickButtonState_[e.jbutton.which][idx]) {
                        isQuitCombo = false;
                        break;
                    }
                }
            }

            if (isQuitCombo) {
                std::chrono::high_resolution_clock::time_point earliest, latest;
                bool firstBtn = true;
                for (int idx : quitComboIndices_) {
                    const auto& t = joystickButtonTimeState_[e.jbutton.which][idx];
                    if (firstBtn) { earliest = latest = t; firstBtn = false; }
                    else { if (t < earliest) earliest = t; if (t > latest) latest = t; }
                }

                if (std::chrono::duration_cast<std::chrono::milliseconds>(latest - earliest).count() <= 200) {
                    if (!anyInputRegistered_) {
                        firstInputWasQuit_ = true;
                        LOG_INFO("InputMonitor", "Quit combo detected (first input).");
                    }
                    else {
                        LOG_INFO("InputMonitor", "Quit combo detected, but it was not the first input.");
                    }
                    anyInputRegistered_ = true;
                    return InputDetectionResult::QuitInput;
                }
            }

            bool isComboButton = false;
            for (int idx : quitComboIndices_) {
                if (buttonIdx == idx) {
                    isComboButton = true;
                    break;
                }
            }

            if (!isComboButton) {
                if (!anyInputRegistered_) {
                    LOG_INFO("InputMonitor", "Generic joystick input detected (non-combo button). This is a 'Play' action.");
                }
                anyInputRegistered_ = true;
                return InputDetectionResult::PlayInput;
            }
        }
        else if (e.type == SDL_JOYBUTTONUP) {
            joystickButtonState_[e.jbutton.which][e.jbutton.button] = false;
        }
    }

    return InputDetectionResult::NoInput;
}