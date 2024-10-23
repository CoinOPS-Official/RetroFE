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

#include "FontCache.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "Font.h"
#if (__APPLE__)
#include <SDL2_ttf/SDL_ttf.h>
#else
#include <SDL2/SDL_ttf.h>
#endif
#include <sstream>

// todo: memory leak when launching games
FontCache::FontCache() = default;

FontCache::~FontCache()
{
    deInitialize();
}

void FontCache::deInitialize()
{
    for (auto it = fontFaceMap_.begin(); it != fontFaceMap_.end(); /* no increment */)
    {
        delete it->second;
        it = fontFaceMap_.erase(it);
    }
    SDL_LockMutex(SDL::getMutex());
    TTF_Quit();
    SDL_UnlockMutex(SDL::getMutex());
}

bool FontCache::initialize() const
{
    if (TTF_Init() == 0)
    {
        return true;
    }
    else
    {
        LOG_WARNING("FontCache", "TTF_Init failed: " + std::string(TTF_GetError()));
        return false;
    }
}

Font *FontCache::getFont(std::string fontPath, int fontSize, SDL_Color color)
{
    std::string key = buildFontKey(fontPath, fontSize, color);
    auto it = fontFaceMap_.find(key);

    if (it != fontFaceMap_.end())
    {
        return it->second;
    }

    return nullptr;
}

std::string FontCache::buildFontKey(std::string font, int fontSize, SDL_Color color)
{
    std::stringstream ss;
    ss << font << "_SIZE=" << fontSize << " RGB=" << color.r << "." << color.g << "." << color.b;

    return ss.str();
}

bool FontCache::loadFont(std::string fontPath, int fontSize, SDL_Color color, int monitor)
{
    std::string key = buildFontKey(fontPath, fontSize, color);
    auto it = fontFaceMap_.find(key);

    if (it == fontFaceMap_.end())
    {
        Font *f = new Font(fontPath, fontSize, color, monitor);
        if (f->initialize())
        {
            fontFaceMap_[key] = f;
        }
        else
        {
            delete f;
            return false;
        }
    }

    return true;
}
