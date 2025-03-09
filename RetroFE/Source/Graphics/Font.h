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

#include <SDL2/SDL.h>
#if (__APPLE__)
#include <SDL2_ttf/SDL_ttf.h>
#else
#include <SDL2/SDL_ttf.h>
#endif
#include <string>
#include <unordered_map>

class FontManager
{
public:
    struct GlyphInfo
    {
        int minX;
        int maxX;
        int minY;
        int maxY;
        int advance;
        SDL_Rect rect;
    };

    FontManager(std::string fontPath, int fontSize, SDL_Color color, int monitor);
    virtual ~FontManager();
    bool initialize();
    void deInitialize();
    SDL_Texture *getTexture();
    bool getRect(unsigned int charCode, GlyphInfo &glyph);
    int getHeight() const;
    int getWidth(const std::string& text);
    int getFontSize() const;
    int getAscent() const;
    int getDescent() const;

private:
    struct GlyphInfoBuild
    {
        FontManager::GlyphInfo glyph;
        SDL_Surface *surface;
    };

    TTF_Font* font_;

    SDL_Texture *texture;
    int height;
    int descent;
    int ascent;
    std::unordered_map<unsigned int, GlyphInfoBuild*> atlas;
    std::string fontPath_;
    int fontSize_;
    SDL_Color color_;
    int monitor_;
};