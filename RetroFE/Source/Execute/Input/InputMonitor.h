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

#include <SDL3/SDL.h>

#include "SDLJoyStickScopeGuard.h"
#include "KeyboardBackendFactory.h"
#include "../../Database/Configuration.h" // For reading config values
#include "../../Utility/Utils.h"         // For listToVector
#include "../../Utility/Log.h"

enum class InputDetectionResult {
    NoInput,
    PlayInput,
    QuitInput
};

class InputMonitor {
public:
    explicit InputMonitor(Configuration& config);

    InputDetectionResult checkInputEvents();

    bool wasQuitFirstInput() const {
        return firstInputWasQuit_;
    }

    void reset() {
        joystickButtonState_.clear();
        joystickButtonTimeState_.clear();
        kbPressed_.clear();
        kbDownTs_.clear();
        anyInputRegistered_ = false;
        firstInputWasQuit_ = false;
    }

    InputMonitor(const InputMonitor&) = delete;
    InputMonitor& operator=(const InputMonitor&) = delete;

private:
    // The monitor and its event loop belong to the SDL main thread.
    std::unique_ptr<SDLJoystickScopeGuard> sdlSession_;
    std::unique_ptr<IKeyboardBackend> kb_;
    std::vector<int> kbSingles_, kbCombo_;
    std::unordered_set<int> kbPressed_;
    std::unordered_map<int, int64_t> kbDownTs_;
    static int64_t nowMs();

    InputDetectionResult pollKeyboard_();
    InputDetectionResult pollSdlEvents_(); // Renamed from pollJoystickSDL_

    std::set<int> singleQuitButtonIndices_;
    std::vector<int> quitComboIndices_;

    bool sdlGameController_;

    std::map<SDL_JoystickID, std::map<int, bool>> joystickButtonState_;
    std::map<SDL_JoystickID, std::map<int, std::chrono::high_resolution_clock::time_point>> joystickButtonTimeState_;

    bool anyInputRegistered_ = false;
    bool firstInputWasQuit_ = false;
};