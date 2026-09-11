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

#include "GameControllerAxisHandler.h"

GameControllerAxisHandler::GameControllerAxisHandler(int joyid, SDL_GamepadAxis axis, Sint16 min, Sint16 max)
    : joyid_(joyid)
    , axis_(axis)
    , min_(min)
    , max_(max)
{
}

void GameControllerAxisHandler::reset()
{
    pressed_ = false;
}

bool GameControllerAxisHandler::update(SDL_Event &e)
{
    if (e.type != SDL_EVENT_GAMEPAD_AXIS_MOTION || (joyid_ != -1 && e.gaxis.which != joyid_) || static_cast<SDL_GamepadAxis>(e.gaxis.axis) != axis_) return false;
    pressed_ = (min_ <= e.gaxis.value && e.gaxis.value <= max_);

    return true;
}

bool GameControllerAxisHandler::pressed()
{
    return pressed_;
}
