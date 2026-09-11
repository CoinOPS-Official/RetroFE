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
#include "../../Database/GlobalOpts.h"
#include <chrono>

static inline int64_t ms_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int64_t InputMonitor::nowMs() { return ms_now(); }


InputMonitor::InputMonitor(Configuration& config) {
    // Create platform-specific backend for keyboard polling
    kb_ = makeKeyboardBackend();

    // Check if GameController mode is enabled to allow semantic parsing
    sdlGameController_ = false;
    config.getProperty(OPTION_SDLGAMECONTROLLER, sdlGameController_);
    sdlSession_ = std::make_unique<SDLJoystickScopeGuard>(sdlGameController_);

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
            std::string sigLower = Utils::toLower(signal);

            if (sigLower.rfind("joybutton", 0) == 0) {
                try {
                    int idx = std::stoi(signal.substr(9));
                    singleQuitButtonIndices_.insert(idx);
                }
                catch (...) {}
            }
            else if (sigLower.rfind("gamepad", 0) == 0 && sdlGameController_) {
                std::string btnName = Utils::replace(sigLower, "gamepad", "");
                // Handle optional player index (e.g., gamepad0back)
                if (!btnName.empty() && isdigit(btnName[0])) btnName.erase(0, 1);

                SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(btnName.c_str());
                if (btn != SDL_GAMEPAD_BUTTON_INVALID) {
                    singleQuitButtonIndices_.insert(static_cast<int>(btn));
                    LOG_DEBUG("InputMonitor", "Registered gamepad single quit button: " + btnName);
                }
            }
            else {
                int keyCode = kb_ ? kb_->mapKeyName(signal) : -1;
                if (keyCode >= 0) kbSingles_.push_back(keyCode);
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
            std::string sigLower = Utils::toLower(signal);

            if (sigLower.rfind("joybutton", 0) == 0) {
                try {
                    int idx = std::stoi(signal.substr(9));
                    quitComboIndices_.push_back(idx);
                }
                catch (...) {}
            }
            else if (sigLower.rfind("gamepad", 0) == 0 && sdlGameController_) {
                std::string btnName = Utils::replace(sigLower, "gamepad", "");
                if (!btnName.empty() && isdigit(btnName[0])) btnName.erase(0, 1);

                SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(btnName.c_str());
                if (btn != SDL_GAMEPAD_BUTTON_INVALID) {
                    quitComboIndices_.push_back(static_cast<int>(btn));
                    LOG_DEBUG("InputMonitor", "Registered gamepad quit combo button: " + btnName);
                }
            }
            else {
                int keyCode = kb_ ? kb_->mapKeyName(signal) : -1;
                if (keyCode >= 0) kbCombo_.push_back(keyCode);
            }
        }
    }

    if (kb_) {
        kb_->setSingleQuitKeys(kbSingles_);
        kb_->setComboQuitKeys(kbCombo_);
    }
}

InputDetectionResult InputMonitor::checkInputEvents() {
    // Keyboard first so global quit works even if SDL window isn�t focused
    auto k = pollKeyboard_();
    if (k == InputDetectionResult::QuitInput) return k;

    auto s = pollSdlEvents_(); // Now checks Mouse and Joystick
    if (s == InputDetectionResult::QuitInput) return s;

    if (k == InputDetectionResult::PlayInput || s == InputDetectionResult::PlayInput)
        return InputDetectionResult::PlayInput;

    return InputDetectionResult::NoInput;
}

InputDetectionResult InputMonitor::pollSdlEvents_() {
    if (!SDL_IsMainThread() || !sdlSession_->initialized_by_me)
        return InputDetectionResult::NoInput;
    SDL_Event e;
    bool sawPlayActivity = false;
    bool firedQuit = false;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
            sdlSession_->open(e.jdevice.which);
            continue;
        }
        if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
            sdlSession_->remove(e.jdevice.which);
            joystickButtonState_.erase(e.jdevice.which);
            joystickButtonTimeState_.erase(e.jdevice.which);
            continue;
        }
        if (!sdlGameController_ && (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                                   e.type == SDL_EVENT_GAMEPAD_BUTTON_UP)) continue;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            anyInputRegistered_ = true;
            sawPlayActivity = true;
        }
        else if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            // If GameController mode is on, ignore raw JOY events for devices recognized as Controllers.
            // SDL_GetGamepadFromID returns a pointer if the joystick is currently open
            // as a GameController in this process.
            if (sdlGameController_ && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
                if (SDL_GetGamepadFromID(e.jbutton.which) != nullptr) {
                    continue;
                }
            }

            int buttonIdx = -1;
            SDL_JoystickID joyId = 0;

            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                buttonIdx = e.gbutton.button;
                joyId = e.gbutton.which;
            }
            else {
                buttonIdx = e.jbutton.button;
                joyId = e.jbutton.which;
            }

            // 1. Single Quit
            if (singleQuitButtonIndices_.count(buttonIdx) > 0) {
                if (!anyInputRegistered_) {
                    firstInputWasQuit_ = true;
                    LOG_INFO("InputMonitor", "Gamepad single quit (first input).");
                }
                firedQuit = true;
                anyInputRegistered_ = true;
            }
            else {
                joystickButtonState_[joyId][buttonIdx] = true;
                joystickButtonTimeState_[joyId][buttonIdx] = std::chrono::high_resolution_clock::now();

                // 2. Combo Check
                bool isComboPart = false;
                for (int idx : quitComboIndices_) if (buttonIdx == idx) { isComboPart = true; break; }

                if (!isComboPart) {
                    anyInputRegistered_ = true;
                    sawPlayActivity = true;
                }
                else {
                    bool allDown = true;
                    std::chrono::high_resolution_clock::time_point earliest, latest;
                    bool firstBtn = true;
                    for (int idx : quitComboIndices_) {
                        if (!joystickButtonState_[joyId][idx]) { allDown = false; break; }
                        auto t = joystickButtonTimeState_[joyId][idx];
                        if (firstBtn) { earliest = latest = t; firstBtn = false; }
                        else { if (t < earliest) earliest = t; if (t > latest) latest = t; }
                    }

                    if (allDown && std::chrono::duration_cast<std::chrono::milliseconds>(latest - earliest).count() <= 200) {
                        if (!anyInputRegistered_) {
                            firstInputWasQuit_ = true;
                            LOG_INFO("InputMonitor", "Gamepad quit combo (first input).");
                        }
                        firedQuit = true;
                        anyInputRegistered_ = true;
                    }
                }
            }
        }
        else if (e.type == SDL_EVENT_JOYSTICK_BUTTON_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            if (sdlGameController_ && e.type == SDL_EVENT_JOYSTICK_BUTTON_UP) {
                if (SDL_GetGamepadFromID(e.jbutton.which) != nullptr) {
                    continue;
                }
            }

            int buttonIdx = -1;
            SDL_JoystickID joyId = 0;

            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
                buttonIdx = e.gbutton.button;
                joyId = e.gbutton.which;
            }
            else {
                buttonIdx = e.jbutton.button;
                joyId = e.jbutton.which;
            }

            bool isComboPart = false;
            for (int idx : quitComboIndices_) if (buttonIdx == idx) { isComboPart = true; break; }
            if (isComboPart) anyInputRegistered_ = true;

            joystickButtonState_[joyId][buttonIdx] = false;
        }
    }

    if (firedQuit) return InputDetectionResult::QuitInput;
    return sawPlayActivity ? InputDetectionResult::PlayInput : InputDetectionResult::NoInput;
}
InputDetectionResult InputMonitor::pollKeyboard_() {
    if (!kb_) return InputDetectionResult::NoInput;

    bool sawPlayActivity = false;
    bool firedSingleQuit = false;

    kb_->poll([&](int code, bool down) {
        if (down) {
            kbPressed_.insert(code);
            kbDownTs_[code] = nowMs();

            // 1. Check if it's a single quit key
            bool isSingle = false;
            for (int s : kbSingles_) if (s == code) { isSingle = true; break; }

            if (isSingle) {
                if (!anyInputRegistered_) {
                    firstInputWasQuit_ = true;
                    LOG_INFO("InputMonitor", "Single quit key (first input).");
                }
                firedSingleQuit = true;
                anyInputRegistered_ = true;
            }
            else {
                // 2. Check if it's part of a combo
                bool isComboPart = false;
                for (int c : kbCombo_) if (c == code) { isComboPart = true; break; }

                // If it's NOT a combo key and NOT a single quit, it's definitely PLAY
                if (!isComboPart) {
                    anyInputRegistered_ = true;
                    sawPlayActivity = true;
                }
            }
        }
        else {
            // Key release: if it was a combo key that was never finished,
            // the user interacted with the game, so it counts as activity now.
            bool isComboPart = false;
            for (int c : kbCombo_) if (c == code) { isComboPart = true; break; }
            if (isComboPart) anyInputRegistered_ = true;

            kbPressed_.erase(code);
        }
        });

    if (firedSingleQuit) return InputDetectionResult::QuitInput;

    // 3. Evaluate Combo
    if (!kbCombo_.empty()) {
        bool allDown = true;
        int64_t earliest = INT64_MAX, latest = 0;
        for (int k : kbCombo_) {
            if (kbPressed_.find(k) == kbPressed_.end()) { allDown = false; break; }
            earliest = std::min(earliest, kbDownTs_[k]);
            latest = std::max(latest, kbDownTs_[k]);
        }

        if (allDown && (latest - earliest) <= 200) {
            if (!anyInputRegistered_) {
                firstInputWasQuit_ = true;
                LOG_INFO("InputMonitor", "Quit combo (first input).");
            }
            anyInputRegistered_ = true;
            return InputDetectionResult::QuitInput;
        }
    }

    if (sawPlayActivity) return InputDetectionResult::PlayInput;
    return kbPressed_.empty() ? InputDetectionResult::NoInput : InputDetectionResult::PlayInput;
}