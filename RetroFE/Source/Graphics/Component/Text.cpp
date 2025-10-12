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

    // --- NEW: Select the best MipLevel for the current render size ---
    const int targetFontSize = static_cast<int>(baseViewInfo.FontSize);
    const FontManager::MipLevel* mip = font->getMipLevelForSize(targetFontSize);
    if (!mip || !mip->fillTexture) {
        // If no suitable mip is found, we cannot draw.
        return;
    }

    // MODIFIED: Get textures from the selected mip level.
    SDL_Texture* fillTex = mip->fillTexture;
    SDL_Texture* outlineTex = mip->outlineTexture;

    // MODIFIED: The scale is now calculated relative to the chosen mip's height,
    // which results in a much higher quality downscale.
    const float mipImageHeight = float(mip->height);
    const float scale = (mipImageHeight > 0.f) ? (baseViewInfo.FontSize / mipImageHeight) : 1.f;

    const float maxW =
        (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    if (needsUpdate_ || lastScale_ != scale || lastMaxWidth_ != maxW) {
        // Pass the new mip-relative scale to the update function.
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
    baseViewInfo.ImageWidth = float(mip->atlasW);  // MODIFIED: use mip's atlas size
    baseViewInfo.ImageHeight = float(mip->atlasH); // MODIFIED: use mip's atlas size

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

            // MODIFIED: Get glyph info from the mip level's map.
            auto it = mip->glyphs.find(ch);
            if (it == mip->glyphs.end()) continue;
            const FontManager::GlyphInfo& g = it->second;

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

            // MODIFIED: Get glyph info from the mip level's map.
            auto it = mip->glyphs.find(ch);
            if (it == mip->glyphs.end()) continue;
            const FontManager::GlyphInfo& g = it->second;

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

    const int targetFontSize = static_cast<int>(baseViewInfo.FontSize);
    const FontManager::MipLevel* mip = font->getMipLevelForSize(targetFontSize);
    if (!mip) return;

    const float ascent_f = static_cast<float>(mip->ascent);

    const float kerningScale =
        (font->getMaxFontSize() > 0)
        ? static_cast<float>(targetFontSize) / static_cast<float>(font->getMaxFontSize())
        : 1.0f;

    // Use double for running math to reduce accumulation error
    double penX = 0.0;
    Uint16 prev = 0;

    struct PosTmp { SDL_Rect src; float xOff, yOff, advance_px; };
    std::vector<PosTmp> tmp; tmp.reserve(textData_.size());

    for (unsigned char uc : textData_) {
        const Uint16 ch = static_cast<Uint16>(uc);

        auto it = mip->glyphs.find(ch);
        if (it == mip->glyphs.end() || it->second.rect.h <= 0) { prev = 0; continue; }
        const auto& g = it->second;

        // kerning from max-res (int) -> screen px
        const int   kern_fp = font->getKerning(prev, ch);
        const float kern_px = static_cast<float>(kern_fp) * kerningScale;

        // positions in screen px (explicit casts for clarity)
        const float minX_px = static_cast<float>(g.minX) * scale;
        const float maxY_px = static_cast<float>(g.maxY) * scale;

        const float gx = static_cast<float>(penX) + kern_px + minX_px;
        const float gy = (ascent_f * scale) - maxY_px;

        const float adv_px = static_cast<float>(g.advance) * scale + kern_px;
        const double nextPen = penX + static_cast<double>(adv_px);

        if (maxWidth > 0.0f && static_cast<float>(nextPen) > maxWidth) break;

        tmp.push_back({ g.rect, gx, gy, adv_px });
        penX = nextPen;
        prev = ch;
    }

    // Commit
    cachedPositions_.reserve(tmp.size());
    for (auto& t : tmp) cachedPositions_.push_back({ t.src, t.xOff, t.yOff, t.advance_px });

    cachedWidth_ = static_cast<float>(penX);
    cachedHeight_ = baseViewInfo.FontSize;
}
