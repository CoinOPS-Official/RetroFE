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


#include "Text.h"
#include "../../Utility/Log.h"
#include "../../SDL.h"
#include "../Font.h"
#include <sstream>


Text::Text( const std::string& text, Page &p, FontManager *font, int monitor )
    : Component(p)
    , textData_(text)
    , fontInst_(font)
    , cachedPositions_()
    , needsUpdate_(true)
{
    baseViewInfo.Monitor = monitor;
    baseViewInfo.Layout = page.getCurrentLayout();
}

Text::~Text() { Component::freeGraphicsMemory(); }



void Text::deInitializeFonts( )
{
    fontInst_->deInitialize( );
}

void Text::initializeFonts( )
{
    fontInst_->initialize( );
}

void Text::setText(const std::string& text, int id) {
    if (getId() == id && textData_ != text) {
        textData_ = text;
        needsUpdate_ = true;
    }
}

const std::string& Text::getText() const {
    return textData_;
}

void Text::draw() {
    Component::draw();

    // --- 1. Setup and Texture Acquisition (Unchanged) ---
    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    SDL_Texture* texture = font ? font->getTexture() : nullptr;
    if (!texture || textData_.empty()) return;

    // --- 2. Glyph Position Caching (Unchanged) ---
    // This section correctly caches the relative positions of glyphs.
    const float imageHeight = float(font->getHeight());
    const float scale = (imageHeight > 0.f) ? (baseViewInfo.FontSize / imageHeight) : 1.f;
    const float maxW =
        (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    if (needsUpdate_ || lastScale_ != scale || lastMaxWidth_ != maxW) {
        updateGlyphPositions(font, scale, maxW);
        needsUpdate_ = false;
        lastScale_ = scale;
        lastMaxWidth_ = maxW;
    }

    // --- 3. Calculate Text Block Origin & Prepare ViewInfo (Mostly Unchanged) ---
    // We must temporarily modify the ViewInfo to calculate the origin of the entire
    // text block and to pass the atlas dimensions to the renderer.
    const float oldW = baseViewInfo.Width;
    const float oldH = baseViewInfo.Height;
    const float oldIW = baseViewInfo.ImageWidth;
    const float oldIH = baseViewInfo.ImageHeight;

    // Set the text's computed size to find its top-left origin point.
    baseViewInfo.Width = cachedWidth_;
    baseViewInfo.Height = baseViewInfo.FontSize;

    baseViewInfo.ImageWidth = float(font->getAtlasW());
    baseViewInfo.ImageHeight = float(font->getAtlasH());

    const float xOrigin = baseViewInfo.XRelativeToOrigin();
    const float yOrigin = baseViewInfo.YRelativeToOrigin();

    // Restore layout-related sizes immediately after use.
    baseViewInfo.Width = oldW;
    baseViewInfo.Height = oldH;

    // --- 4. Get Layout Dimensions (Simplified) ---
    const int m = baseViewInfo.Monitor;
    const int layoutW = page.getLayoutWidthByMonitor(m);
    const int layoutH = page.getLayoutHeightByMonitor(m);

    SDL_FRect destRect; // Re-used for each glyph
    for (const auto& pos : cachedPositions_) {
        // Construct the LOGICAL destination rectangle for this single glyph.
        // It's the text block's origin plus the glyph's relative offset.
        destRect.x = xOrigin + pos.xOffset;
        destRect.y = yOrigin + pos.yOffset;
        destRect.w = pos.sourceRect.w * scale;
        destRect.h = pos.sourceRect.h * scale;

        // Call the master renderer for each glyph. It will handle scaling,
        // rotation, mirroring, and final drawing.
        SDL::renderCopyF(
            texture,
            baseViewInfo.Alpha,
            &pos.sourceRect, // The glyph's rect in the atlas (Source)
            &destRect,       // The glyph's logical position on screen (Destination)
            baseViewInfo,    // Contains alpha, angle, and atlas dimensions
            layoutW,
            layoutH
        );
    }

    // --- 6. Restore ViewInfo (Unchanged) ---
    // Restore the original ImageWidth/Height so we don't affect other components.
    baseViewInfo.ImageWidth = oldIW;
    baseViewInfo.ImageHeight = oldIH;
}

void Text::updateGlyphPositions(FontManager* font, float scale, float maxWidth) {
    cachedPositions_.clear();
    cachedPositions_.reserve(textData_.size());

    float currentWidth = 0.0f;
    int baseAscent = font->getAscent();

    for (char c : textData_) {
        FontManager::GlyphInfo glyph;
        if (!font->getRect(c, glyph) || glyph.rect.h <= 0) continue;

        // Adjust currentWidth by glyph.minX if minX < 0, unscaled
        if (glyph.minX < 0) {
            currentWidth += glyph.minX;
        }

        // Check if adding the glyph exceeds maxWidth
        float scaledCurrentWidth = currentWidth * scale;
        float scaledAdvance = glyph.advance * scale;

        if ((scaledCurrentWidth + scaledAdvance) > maxWidth) break;

        // Calculate xOffset
        int xOffset = static_cast<int>(scaledCurrentWidth);
        if (glyph.minX < 0) {
            xOffset += static_cast<int>(glyph.minX * scale);
        }

        int yOffset = baseAscent < glyph.maxY ? static_cast<int>((baseAscent - glyph.maxY) * scale) : 0;

        cachedPositions_.push_back({
            glyph.rect,
            xOffset,
            yOffset,
            scaledAdvance
            });

        // Increment currentWidth by glyph.advance, unscaled
        currentWidth += glyph.advance;
    }

    cachedWidth_ = currentWidth * scale;
    cachedHeight_ = baseViewInfo.FontSize;
}