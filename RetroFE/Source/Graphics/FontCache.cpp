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
#if __has_include(<SDL2/SDL_ttf.h>)
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif
#include <sstream>
#include <memory>

FontCache::FontCache() = default;

FontCache::~FontCache()
{
    deInitialize();
}

void FontCache::deInitialize() {
    fontFaceMap_.clear(); // With smart pointers, no need for explicit delete calls
    TTF_Quit();
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

FontManager* FontCache::getFont(const std::string& fontPath, int fontSize, SDL_Color color, bool gradient, int outlinePx, int monitor) {
    std::string key = buildFontKey(fontPath, fontSize, color, gradient, outlinePx, monitor);
    auto it = fontFaceMap_.find(key);

    if (it != fontFaceMap_.end()) {
        return it->second.get(); // Access the raw pointer from unique_ptr
    }

    return nullptr;
}


std::string FontCache::buildFontKey(std::string font, int fontSize, SDL_Color color, bool gradient, int outlinePx, int monitor)
{
    std::stringstream ss;
    ss << font << "_SIZE=" << fontSize << " RGB=" << color.r << "." << color.g << "." << color.b;
    ss << "_MONITOR=" << monitor;
	ss << (gradient ? "_GRADIENT" : "");
	ss << "_OUTLINE=" << outlinePx;
    return ss.str();
}

bool FontCache::loadFont(std::string fontPath, int fontSize, SDL_Color color, bool gradient, int outlinePx, int monitor) {
    std::string key = buildFontKey(fontPath, fontSize, color, gradient, outlinePx, monitor);
    if (fontFaceMap_.find(key) == fontFaceMap_.end()) {
        auto font = std::make_unique<FontManager>(fontPath, fontSize, color, gradient, outlinePx, monitor);
        if (font->initialize()) {
            fontFaceMap_[key] = std::move(font);
        } else {
            return false;
        }
    }
    return true;
}