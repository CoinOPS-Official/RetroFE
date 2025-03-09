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

#include "Font.h"
#include <string>
#include <unordered_map>
#include <memory>

class FontCache
{
public:
    FontCache();
    bool initialize() const;
    void deInitialize();
    bool loadFont(std::string font, int fontSize, SDL_Color color, int monitor);
    FontManager* getFont(const std::string& fontPath, int fontSize, SDL_Color color, int monitor);

    virtual ~FontCache();
private:
    std::unordered_map<std::string, std::unique_ptr<FontManager>> fontFaceMap_;
    std::string buildFontKey(std::string font, int fontSize, SDL_Color color, int monitor);

};

