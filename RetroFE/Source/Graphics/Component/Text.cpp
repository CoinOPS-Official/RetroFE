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
    if (!font || textData_.empty()) return;

    SDL_Texture* fillTex = font->getTexture();         // fill atlas (tinted)
    SDL_Texture* outlineTex = font->getOutlineTexture();  // may be nullptr
    if (!fillTex) return;

    // --- caching unchanged ---
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
    if (cachedPositions_.empty()) return;

    // --- compute origin (unchanged) ---
    const float oldW = baseViewInfo.Width;
    const float oldH = baseViewInfo.Height;
    const float oldIW = baseViewInfo.ImageWidth;
    const float oldIH = baseViewInfo.ImageHeight;

    baseViewInfo.Width = cachedWidth_;
    baseViewInfo.Height = baseViewInfo.FontSize;
    baseViewInfo.ImageWidth = float(font->getAtlasW());
    baseViewInfo.ImageHeight = float(font->getAtlasH());

    const float xOrigin = baseViewInfo.XRelativeToOrigin();
    const float yOrigin = baseViewInfo.YRelativeToOrigin();

    baseViewInfo.Width = oldW;
    baseViewInfo.Height = oldH;

    const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
    const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);

    // --- PASS 1: OUTLINE — anchor by subtracting fill offset ---
    if (outlineTex) {
        SDL_FRect dst;
        size_t i = 0;
        for (unsigned char uc : textData_) {
            if (i >= cachedPositions_.size()) break;
            const auto& pos = cachedPositions_[i++];
            Uint16 ch = (Uint16)uc;

            FontManager::GlyphInfo g;
            if (!font->getRect(ch, g)) continue;

            // packed (outline) source
            const SDL_Rect& srcOutline = g.rect;

            // destination: start from fill top-left, then shift back by fill offset
            dst.x = xOrigin + pos.xOffset - g.fillX * scale;
            dst.y = yOrigin + pos.yOffset - g.fillY * scale;
            dst.w = srcOutline.w * scale;
            dst.h = srcOutline.h * scale;

            SDL::renderCopyF(
                outlineTex,
                baseViewInfo.Alpha,
                &srcOutline,
                &dst,
                baseViewInfo,
                layoutW, layoutH
            );
        }
    }

    // --- PASS 2: FILL — anchor directly at fill top-left (no extra +fillX/+fillY) ---
    {
        SDL_FRect dst;
        size_t i = 0;
        for (unsigned char uc : textData_) {
            if (i >= cachedPositions_.size()) break;
            const auto& pos = cachedPositions_[i++];
            Uint16 ch = (Uint16)uc;

            FontManager::GlyphInfo g;
            if (!font->getRect(ch, g)) continue;

            SDL_Rect srcFill{
                g.rect.x + g.fillX,
                g.rect.y + g.fillY,
                g.fillW,
                g.fillH
            };

            // Position is already the fill's top-left
            dst.x = xOrigin + pos.xOffset;
            dst.y = yOrigin + pos.yOffset;
            dst.w = g.fillW * scale;
            dst.h = g.fillH * scale;

            SDL::renderCopyF(
                fillTex,
                baseViewInfo.Alpha,
                &srcFill,
                &dst,
                baseViewInfo,
                layoutW, layoutH
            );
        }
    }

    baseViewInfo.ImageWidth = oldIW;
    baseViewInfo.ImageHeight = oldIH;
}

void Text::updateGlyphPositions(FontManager* font, float scale, float maxWidth) {
    cachedPositions_.clear();
    cachedPositions_.reserve(textData_.size());

    const int ascent = font->getAscent();

    float penX_px = 0.0f;
    Uint16 prev = 0;
    int minYOffset = INT_MAX;

    struct PosTmp {
        SDL_Rect src;
        int xOff, yOff;
        float advance_px;
    };
    std::vector<PosTmp> tmp; tmp.reserve(textData_.size());

    for (unsigned char uc : textData_) {
        Uint16 ch = (Uint16)uc;
        FontManager::GlyphInfo g;
        if (!font->getRect(ch, g) || g.rect.h <= 0) { prev = 0; continue; }

        int kern_fp = font->getKerning(prev, ch);
        float kern_px = kern_fp * scale;

        float gx = penX_px + kern_px + g.minX * scale;
        float gy = (ascent - g.maxY) * scale;

        int xOffset = (int)std::lround(gx);
        int yOffset = (int)std::lround(gy);

        float nextPen_px = penX_px + (g.advance * scale) + kern_px;
        if (maxWidth > 0.f && (nextPen_px > maxWidth)) break;

        tmp.push_back({ g.rect, xOffset, yOffset, (g.advance * scale) + kern_px });
        if (yOffset < minYOffset) minYOffset = yOffset;

        penX_px = nextPen_px;
        prev = ch;
    }

    // Normalize Y so tallest glyph top = 0 (matches legacy visual)
    if (minYOffset != INT_MAX && minYOffset != 0) {
        for (auto& t : tmp) t.yOff -= minYOffset;
    }

    cachedPositions_.reserve(tmp.size());
    for (auto& t : tmp) cachedPositions_.push_back({ t.src, t.xOff, t.yOff, t.advance_px });

    cachedWidth_ = penX_px;
    cachedHeight_ = baseViewInfo.FontSize;
}
