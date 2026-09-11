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

#include "GameControllerButtonHandler.h"

GameControllerButtonHandler::GameControllerButtonHandler(int joynum, SDL_GamepadButton button)
    : joynum_(joynum)
    , button_(button)
{
}

void GameControllerButtonHandler::reset()
{
    pressed_ = false;
}

bool GameControllerButtonHandler::update(SDL_Event &e)
{
    if (e.type != SDL_EVENT_GAMEPAD_BUTTON_UP && e.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) return false;

    if ((joynum_ == -1 || e.gbutton.which == joynum_) && static_cast<SDL_GamepadButton>(e.gbutton.button) == button_) {
        pressed_ = (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }

    return false;
}

bool GameControllerButtonHandler::pressed()
{
    return pressed_;
}
