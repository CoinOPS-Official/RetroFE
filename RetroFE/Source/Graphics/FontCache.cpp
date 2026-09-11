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

FontManager* FontCache::getFont(const std::string& fontPath, int maxFontSize, bool gradient, int outlinePx, int monitor) {
    // 1. First, check for a perfect, exact configuration match (Fast path)
    std::string exactKey = buildFontKey(fontPath, maxFontSize, gradient, outlinePx, monitor);
    auto it = fontFaceMap_.find(exactKey);
    if (it != fontFaceMap_.end()) {
        return it->second.get();
    }

    // 2. Fallback: Search for an existing larger font that can satisfy this request via mipmapping
    FontManager* bestMatch = nullptr;
    int closestLargerSize = std::numeric_limits<int>::max();

    for (const auto& [key, fontPtr] : fontFaceMap_) {
        // FIX: Verify structural layout attributes match perfectly, ignoring color paths completely
        if (fontPtr->getOutlinePx() == outlinePx &&
            fontPtr->getGradient() == gradient &&
            fontPtr->getMonitor() == monitor &&
            fontPtr->getFontPath() == fontPath)
        {
            int existingSize = fontPtr->getMaxFontSize();

            // Is it larger than what we need, but smaller than any other candidate we've seen?
            if (existingSize >= maxFontSize && existingSize < closestLargerSize) {
                closestLargerSize = existingSize;
                bestMatch = fontPtr.get();
            }
        }
    }

    if (bestMatch) {
        LOG_INFO("FontCache", "[PERF] Shared font hit! Using existing size " +
            std::to_string(closestLargerSize) + " to satisfy request for size " + std::to_string(maxFontSize));
        return bestMatch;
    }

    return nullptr;
}

std::string FontCache::buildFontKey(std::string font, int maxFontSize, bool gradient, int outlinePx, int monitor) {
    std::stringstream ss;
    // Fix: Dropped RGB parameters. Color is a rendering property, not a font geometry property.
    ss << font << "_SIZE=" << maxFontSize;
    ss << "_MONITOR=" << monitor;
    ss << (gradient ? "_GRADIENT" : "");
    ss << "_OUTLINE=" << outlinePx;
    return ss.str();
}

bool FontCache::loadFont(std::string fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor) {
    // Check if we already have an identical or larger font available that satisfies this layout component
    if (getFont(fontPath, maxFontSize, gradient, outlinePx, monitor) != nullptr) {
        return true; // Short-circuit completely! A valid target asset handle is already warm
    }

    std::string key = buildFontKey(fontPath, maxFontSize, gradient, outlinePx, monitor);

    // Only compile a cold initialization pass if absolutely no larger matching variant exists
    auto font = std::make_unique<FontManager>(fontPath, maxFontSize, color, gradient, outlinePx, monitor);
    if (font->initialize()) {
        fontFaceMap_[key] = std::move(font);
        return true;
    }

    return false;
}
