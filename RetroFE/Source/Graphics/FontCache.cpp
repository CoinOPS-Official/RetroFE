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
#include "Font.h"
#include "../Utility/Log.h"
#include "../SDL.h"
#if (__APPLE__)
#include <SDL2_ttf/SDL_ttf.h>
#else
#include <SDL2/SDL_ttf.h>
#endif
#include <sstream>

 //todo: memory leak when launching games
FontCache::FontCache() : initialized_(false)
{
}

FontCache::~FontCache()
{
    deInitialize();
}

void FontCache::deInitialize()
{
    SDL_LockMutex(SDL::getMutex());
    for (auto& entry : fontFaceMap_) {
        delete entry.second;
    }
    fontFaceMap_.clear();
    if (initialized_) {
        TTF_Quit();
        initialized_ = false;
    }
    SDL_UnlockMutex(SDL::getMutex());
}

void FontCache::initialize()
{
    SDL_LockMutex(SDL::getMutex());
    if (TTF_Init() == -1) {
        LOG_ERROR("FontCache", "Failed to initialize TTF: " + std::string(TTF_GetError()));
    }
    else {
        initialized_ = true;
    }
    SDL_UnlockMutex(SDL::getMutex());
}

Font* FontCache::getFont(const std::string& fontPath, int fontSize, SDL_Color color)
{
    SDL_LockMutex(SDL::getMutex());
    auto it = fontFaceMap_.find(buildFontKey(fontPath, fontSize, color));
    if (it != fontFaceMap_.end()) {
        SDL_UnlockMutex(SDL::getMutex());
        return it->second;
    }
    SDL_UnlockMutex(SDL::getMutex());
    return nullptr;
}

std::string FontCache::buildFontKey(const std::string& font, int fontSize, SDL_Color color)
{
    std::stringstream ss;
    ss << font << "_SIZE=" << fontSize << " RGB=" << (int)color.r << "." << (int)color.g << "." << (int)color.b;
    return ss.str();
}

bool FontCache::loadFont(const std::string& fontPath, int fontSize, SDL_Color color, int monitor)
{
    SDL_LockMutex(SDL::getMutex());
    std::string key = buildFontKey(fontPath, fontSize, color);
    auto it = fontFaceMap_.find(key);
    if (it == fontFaceMap_.end()) {
        Font* f = new Font(fontPath, fontSize, color, monitor);
        if (f->initialize()) {
            fontFaceMap_[key] = f;
        }
        else {
            delete f;
            SDL_UnlockMutex(SDL::getMutex());
            return false;
        }
    }
    SDL_UnlockMutex(SDL::getMutex());
    return true;
}


