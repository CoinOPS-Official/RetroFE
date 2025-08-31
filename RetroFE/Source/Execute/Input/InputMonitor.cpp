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
#include <chrono>

static IKeyboardBackend * getKeyboardBackendSingleton() {
    static std::unique_ptr<IKeyboardBackend> s = makeKeyboardBackend();
    return s.get();
}

static inline int64_t ms_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int64_t InputMonitor::nowMs() { return ms_now(); }


InputMonitor::InputMonitor(Configuration& config) {
    // Create platform-specific backend for keyboard polling
    kb_ = getKeyboardBackendSingleton();

    // --- Helper to trim whitespace if needed ---
    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };

    // --- Parse the "quit" binding ---
    std::string quitBtnsStr;
    if (config.getProperty("controls.quit", quitBtnsStr)) {
        std::vector<std::string> signals;
        Utils::listToVector(quitBtnsStr, signals, ',');
        for (auto signal : signals) {
            trim(signal);

            if (signal.rfind("joyButton", 0) == 0) {
                // Joystick button
                try {
                    int idx = std::stoi(signal.substr(9));
                    singleQuitButtonIndices_.insert(idx);
                    LOG_DEBUG("InputMonitor", "Registered single quit button index: " + std::to_string(idx));
                }
                catch (const std::exception& e) {
                    LOG_ERROR("InputMonitor", "Failed to parse single quit button: " + signal + " (" + e.what() + ")");
                }
            }
            else {
                // Keyboard key
                int keyCode = kb_ ? kb_->mapKeyName(signal) : -1;
                if (keyCode >= 0) {
                    kbSingles_.push_back(keyCode);
                    LOG_DEBUG("InputMonitor", "Registered single quit keyboard key: " + signal);
                }
                else {
                    LOG_WARNING("InputMonitor", "Unknown keyboard quit key: " + signal);
                }
            }
        }
    }

    // --- Parse the "quitCombo" binding ---
    std::string quitComboStr;
    if (config.getProperty("controls.quitCombo", quitComboStr)) {
        std::vector<std::string> signals;
        Utils::listToVector(quitComboStr, signals, ',');
        for (auto signal : signals) {
            trim(signal);

            if (signal.rfind("joyButton", 0) == 0) {
                // Joystick button combo
                try {
                    int idx = std::stoi(signal.substr(9));
                    quitComboIndices_.push_back(idx);
                    LOG_DEBUG("InputMonitor", "Registered combo quit button index: " + std::to_string(idx));
                }
                catch (const std::exception& e) {
                    LOG_ERROR("InputMonitor", "Failed to parse combo quit button: " + signal + " (" + e.what() + ")");
                }
            }
            else {
                // Keyboard combo key
                int keyCode = kb_ ? kb_->mapKeyName(signal) : -1;
                if (keyCode >= 0) {
                    kbCombo_.push_back(keyCode);
                    LOG_DEBUG("InputMonitor", "Registered combo quit keyboard key: " + signal);
                }
                else {
                    LOG_WARNING("InputMonitor", "Unknown keyboard combo quit key: " + signal);
                }
            }
        }
    }

    // Tell backend which keys to monitor
    if (kb_) {
        kb_->setSingleQuitKeys(kbSingles_);
        kb_->setComboQuitKeys(kbCombo_);
    }
}

InputDetectionResult InputMonitor::checkInputEvents() {
    // Keyboard first so global quit works even if SDL window isn’t focused
    auto k = pollKeyboard_();
    if (k == InputDetectionResult::QuitInput) return k;

    auto j = pollJoystickSDL_();   // same logic you already have
    if (j == InputDetectionResult::QuitInput) return j;

    if (k == InputDetectionResult::PlayInput || j == InputDetectionResult::PlayInput)
        return InputDetectionResult::PlayInput;

    return InputDetectionResult::NoInput;
}

InputDetectionResult InputMonitor::pollJoystickSDL_() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_JOYBUTTONDOWN) {
            int buttonIdx = e.jbutton.button;

            if (singleQuitButtonIndices_.count(buttonIdx) > 0) {
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

            bool isQuitCombo = !quitComboIndices_.empty();
            if (isQuitCombo) {
                for (int idx : quitComboIndices_) {
                    if (joystickButtonState_[e.jbutton.which].count(idx) == 0 ||
                        !joystickButtonState_[e.jbutton.which][idx]) {
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
                if (buttonIdx == idx) { isComboButton = true; break; }
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

InputDetectionResult InputMonitor::pollKeyboard_() {
    if (!kb_) return InputDetectionResult::NoInput;

    bool sawPlay = false;
    bool firedQuit = false;

    kb_->poll([&](int code, bool down) {
        if (down) {
            kbPressed_.insert(code);
            kbDownTs_[code] = nowMs();

            // Single-key quit (edge)
            for (int s : kbSingles_) if (s == code) {
                firedQuit = true;
                if (!anyInputRegistered_) {
                    firstInputWasQuit_ = true;
                    LOG_INFO("InputMonitor", "Keyboard single quit (first input).");
                }
                else {
                    LOG_INFO("InputMonitor", "Keyboard single quit.");
                }
                anyInputRegistered_ = true;
                break;
            }

            // Mark generic Play if this key isn’t part of the combo set
            bool isComboKey = false;
            for (int k : kbCombo_) { if (k == code) { isComboKey = true; break; } }
            if (!isComboKey) sawPlay = true;

        }
        else { // key up
            kbPressed_.erase(code);
            kbDownTs_.erase(code);
        }
        });

    if (firedQuit) return InputDetectionResult::QuitInput;

    // Combo check (?200 ms window), no modifiers
    if (!kbCombo_.empty()) {
        bool allDown = true;
        int64_t earliest = INT64_MAX, latest = 0;
        for (int k : kbCombo_) {
            if (!kbPressed_.count(k)) { allDown = false; break; }
            auto it = kbDownTs_.find(k);
            if (it == kbDownTs_.end()) { allDown = false; break; }
            earliest = std::min(earliest, it->second);
            latest = std::max(latest, it->second);
        }
        if (allDown && (latest - earliest) <= 200) {
            if (!anyInputRegistered_) {
                firstInputWasQuit_ = true;
                LOG_INFO("InputMonitor", "Keyboard quit combo (first input).");
            }
            else {
                LOG_INFO("InputMonitor", "Keyboard quit combo.");
            }
            anyInputRegistered_ = true;
            return InputDetectionResult::QuitInput;
        }
    }

    if (sawPlay) {
        if (!anyInputRegistered_) {
            LOG_INFO("InputMonitor", "Generic keyboard input detected. This is a 'Play' action.");
        }
        anyInputRegistered_ = true;
        return InputDetectionResult::PlayInput;
    }

    return InputDetectionResult::NoInput;
}