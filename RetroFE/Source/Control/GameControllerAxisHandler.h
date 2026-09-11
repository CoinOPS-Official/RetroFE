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

#include "InputHandler.h"

class GameControllerAxisHandler : public InputHandler
{
public:
    GameControllerAxisHandler(int joyid, SDL_GamepadAxis axis, Sint16 min, Sint16 max);
    bool update(SDL_Event &e);
    bool pressed();
    void reset();
    void updateKeystate() {}

private:
    int joyid_;
    SDL_GamepadAxis axis_;
    Sint16 min_;
    Sint16 max_;
    bool pressed_{ false };
};
