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

#include <string>
#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif

class Sound
{
public:
    Sound(std::string file, std::string altfile);
    virtual ~Sound();
    void play();
    bool allocate();
    bool free();
    bool isPlaying();
private:
    std::string file_;
    Mix_Chunk  *chunk_;
    int         channel_;
};
