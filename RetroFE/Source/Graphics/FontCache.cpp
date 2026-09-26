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
#include <SDL3_ttf/SDL_ttf.h>
#include <sstream>
#include <memory>

FontCache::FontCache() = default;

FontCache::~FontCache() {
    deInitialize();
}

void FontCache::deInitialize() {
    fontFaceMap_.clear(); // With smart pointers, no need for explicit delete calls
    TTF_Quit();
}

bool FontCache::initialize() const {
    if (TTF_Init())
    {
        return true;
    }
    else
    {
        LOG_WARNING("FontCache", "TTF_Init failed: " + std::string(SDL_GetError()));
        return false;
    }
}

FontManager* FontCache::getFont(const std::string& fontPath, int maxFontSize, bool gradient, int outlinePx, int monitor, const std::string& fallbackFontPath) {
    // 1. First, check for a perfect, exact configuration match (Fast path)
    std::string exactKey = buildFontKey(fontPath, maxFontSize, gradient, outlinePx, monitor, fallbackFontPath);
    auto it = fontFaceMap_.find(exactKey);
    if (it != fontFaceMap_.end()) {
        return it->second.get();
    }

    // loadFontSize defines default text size and outline proportions. Sharing
    // a different reference size would make those depend on layout parse order.

    return nullptr;
}

std::string FontCache::buildFontKey(std::string font, int maxFontSize, bool gradient, int outlinePx, int monitor, const std::string& fallbackFontPath) {
    std::stringstream ss;
    // Fix: Dropped RGB parameters. Color is a rendering property, not a font geometry property.
    ss << font << "_SIZE=" << maxFontSize;
    ss << "_MONITOR=" << monitor;
    ss << (gradient ? "_GRADIENT" : "");
    ss << "_OUTLINE=" << outlinePx;
    if (!fallbackFontPath.empty()) {
        ss << "_FALLBACK=" << fallbackFontPath;
    }
    return ss.str();
}

bool FontCache::loadFont(std::string fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor, std::string fallbackFontPath) {
    // Check if we already have an identical font available that satisfies this layout component
    if (getFont(fontPath, maxFontSize, gradient, outlinePx, monitor, fallbackFontPath) != nullptr) {
        return true; // Short-circuit completely! A valid target asset handle is already warm
    }

    std::string key = buildFontKey(fontPath, maxFontSize, gradient, outlinePx, monitor, fallbackFontPath);

    // Only compile a cold initialization pass if this reference-size/style combination is not prepared
    auto font = std::make_unique<FontManager>(fontPath, maxFontSize, color, gradient, outlinePx, monitor, fallbackFontPath);
    if (font->initialize()) {
        fontFaceMap_[key] = std::move(font);
        return true;
    }

    return false;
}
