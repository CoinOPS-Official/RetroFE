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

InputDetectionResult InputMonitor::checkSdlEvents() {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_JOYBUTTONDOWN) {
			// --- STEP 1: Always update button state ---
			joystickButtonState_[e.jbutton.which][e.jbutton.button] = true;
			joystickButtonTimeState_[e.jbutton.which][e.jbutton.button] = std::chrono::high_resolution_clock::now();

			// --- STEP 2: Check if a full quit combo is now active ---
			bool isQuitCombo = true;
			if (quitComboIndices_.empty()) {
				isQuitCombo = false;
			}
			else {
				for (int idx : quitComboIndices_) {
					// If a button isn't in the map or is not pressed, the combo is incomplete.
					if (joystickButtonState_[e.jbutton.which].count(idx) == 0 || !joystickButtonState_[e.jbutton.which][idx]) {
						isQuitCombo = false;
						break;
					}
				}
			}

			// If the combo is complete, validate its timing.
			if (isQuitCombo) {
				std::chrono::high_resolution_clock::time_point earliest, latest;
				bool firstBtn = true;
				for (int idx : quitComboIndices_) {
					const auto& t = joystickButtonTimeState_[e.jbutton.which][idx];
					if (firstBtn) {
						earliest = latest = t;
						firstBtn = false;
					}
					else {
						if (t < earliest) earliest = t;
						if (t > latest) latest = t;
					}
				}

				// Check if buttons were pressed close enough together to be considered a deliberate combo.
				if (std::chrono::duration_cast<std::chrono::milliseconds>(latest - earliest).count() <= 200) {
					if (!anyInputRegistered_) {
						firstInputWasQuit_ = true; // This was the very first registered input.
						LOG_INFO("InputMonitor", "Quit combo detected (first input).");
					}
					else {
						LOG_INFO("InputMonitor", "Quit combo detected, but it was not the first input.");
					}
					anyInputRegistered_ = true;
					return InputDetectionResult::QuitInput; // A valid quit combo was detected.
				}
			}

			// --- STEP 3: If it wasn't a quit combo, check for "Play" input ---
			// A "Play" input is any button press that is NOT part of the quit combo.
			// This prevents a single press of a combo button (e.g., BACK) from being treated as "Play".

			bool isComboButton = false;
			for (int idx : quitComboIndices_) {
				if (e.jbutton.button == idx) {
					isComboButton = true;
					break;
				}
			}

			if (!isComboButton) {
				if (!anyInputRegistered_) {
					LOG_INFO("InputMonitor", "Generic joystick input detected (non-combo button). This is a 'Play' action.");
					// NOTE: firstInputWasQuit_ remains false, which is correct.
				}
				anyInputRegistered_ = true;
				return InputDetectionResult::PlayInput; // A generic "play" input was detected.
			}
			// If it WAS a combo button but didn't complete a valid combo, we do nothing and keep polling.
			// This allows the user time to press the other button(s) in the combo.
		}
		else if (e.type == SDL_JOYBUTTONUP) {
			joystickButtonState_[e.jbutton.which][e.jbutton.button] = false;
		}
	}

	// If the event loop completes without returning, no significant input was detected.
	return InputDetectionResult::NoInput;
}