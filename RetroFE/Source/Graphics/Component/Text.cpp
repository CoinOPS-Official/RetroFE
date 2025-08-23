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

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    SDL_Texture* texture = font->getTexture();
    if (!texture || textData_.empty()) return;

    const float imageHeight = float(font->getHeight());
    const float scale = baseViewInfo.FontSize / imageHeight;
    const float imageMaxWidth =
        (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    if (needsUpdate_ || lastScale_ != scale || lastMaxWidth_ != imageMaxWidth) {
        updateGlyphPositions(font, scale, imageMaxWidth);
        needsUpdate_ = false;
        lastScale_ = scale;
        lastMaxWidth_ = imageMaxWidth;
    }

    // Save layout fields we temporarily tweak for origin math
    const float oldWidth = baseViewInfo.Width;
    const float oldHeight = baseViewInfo.Height;
    const float oldImageWidth = baseViewInfo.ImageWidth;   // <-- keep these!
    const float oldImageHeight = baseViewInfo.ImageHeight;

    // Use text block size for origin/alignment ONLY
    baseViewInfo.Width = cachedWidth_;
    baseViewInfo.Height = baseViewInfo.FontSize;

    baseViewInfo.ImageWidth = float(font->getAtlasW());
    baseViewInfo.ImageHeight = float(font->getAtlasH());

    const float xOrigin = baseViewInfo.XRelativeToOrigin();
    const float yOrigin = baseViewInfo.YRelativeToOrigin();

    // Restore layout widths after we computed origins
    baseViewInfo.Width = oldWidth;
    baseViewInfo.Height = oldHeight;
    // Keep ImageWidth/Height set to atlas size for the draw loop,
    // then restore them after rendering (in case other components rely on them)

    SDL_FRect destRect;
    for (const auto& pos : cachedPositions_) {
        destRect.x = std::floor(xOrigin + pos.xOffset);
        destRect.y = std::floor(yOrigin + pos.yOffset);
        destRect.w = pos.sourceRect.w * scale;
        destRect.h = pos.sourceRect.h * scale;

        SDL::renderCopyF(
            texture,
            baseViewInfo.Alpha,
            &pos.sourceRect,
            &destRect,
            baseViewInfo,
            page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
            page.getLayoutHeightByMonitor(baseViewInfo.Monitor)
        );
    }

    // Restore ImageWidth/Height to previous values
    baseViewInfo.ImageWidth = oldImageWidth;
    baseViewInfo.ImageHeight = oldImageHeight;
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