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
    , cachedLayout_()
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

    const int targetFontSize = static_cast<int>(baseViewInfo.FontSize);
    const FontManager::MipLevel* mip = font->getMipLevelForSize(targetFontSize);
    if (!mip || !mip->fillTexture) return;

    const float scale = (mip->fontSize > 0)
        ? (baseViewInfo.FontSize / (float)mip->fontSize)
        : 1.f;

    const float maxW =
        (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    if (needsUpdate_ ||
        cachedFont_ != font ||
        cachedFontGeneration_ != font->getGeneration() ||
        cachedMipFontSize_ != mip->fontSize)
    {
        cachedLayout_ = font->getTextLayout(
            textData_,
            targetFontSize
        );
        needsUpdate_ = false;
        cachedFont_ = font;
        cachedFontGeneration_ = font->getGeneration();
        cachedMipFontSize_ = mip->fontSize;
    }
    if (!cachedLayout_ || cachedLayout_->glyphs.empty()) return;

    const float kerningScale =
        (font->getMaxFontSize() > 0)
        ? static_cast<float>(targetFontSize) /
            static_cast<float>(font->getMaxFontSize())
        : 1.0f;

    std::size_t visibleGlyphCount = 0;
    cachedWidth_ = 0.0f;

    for (const FontManager::PositionedGlyph& cg :
        cachedLayout_->glyphs)
    {
        const float penStart =
            cg.advanceBefore * scale +
            cg.kerningBefore * kerningScale;

        const float nextPen =
            penStart + cg.advance * scale;

        if (maxW > 0.0f && nextPen > maxW) {
            cachedWidth_ = penStart;
            break;
        }

        cachedWidth_ = nextPen;
        ++visibleGlyphCount;
    }

    if (visibleGlyphCount == 0) return;

    const float outline =
        static_cast<float>(font->getOutlinePx());

    cachedHeight_ =
        static_cast<float>(mip->height) * scale +
        2.0f * outline * scale;

    // NEW: Apply on-the-fly texture color modulation before entering loops
    SDL_SetTextureColorMod(mip->fillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
    if (mip->dynamicFillTexture) {
        SDL_SetTextureColorMod(mip->dynamicFillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
    }

    const float oldW = baseViewInfo.Width;
    const float oldH = baseViewInfo.Height;
    const float oldIW = baseViewInfo.ImageWidth;
    const float oldIH = baseViewInfo.ImageHeight;

    baseViewInfo.Width = cachedWidth_;
    baseViewInfo.Height = cachedHeight_;
    baseViewInfo.ImageWidth = float(mip->atlasW);
    baseViewInfo.ImageHeight = float(mip->atlasH);

    const float xOrigin = baseViewInfo.XRelativeToOrigin();
    const float yOrigin = baseViewInfo.YRelativeToOrigin();

    baseViewInfo.Width = oldW;
    baseViewInfo.Height = oldH;

    const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
    const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);

    SDL::beginGeometryBatch(visibleGlyphCount * 2);

    // --- PASS 1: OUTLINE ---
    for (std::size_t i = 0; i < visibleGlyphCount; ++i) {
        const FontManager::PositionedGlyph& cg =
            cachedLayout_->glyphs[i];
        SDL_Texture* outlineTexture = cg.dynamicAtlas
            ? mip->dynamicOutlineTexture
            : mip->outlineTexture;
        if (outlineTexture) {
            const float penStart =
                cg.advanceBefore * scale +
                cg.kerningBefore * kerningScale;

            SDL_FRect dst = {
                xOrigin + penStart - outline * scale,
                yOrigin + cg.packedY * scale,
                cg.srcOutline.w * scale,
                cg.srcOutline.h * scale
            };
            SDL::renderCopyF(outlineTexture, baseViewInfo.Alpha, &cg.srcOutline, &dst, baseViewInfo, layoutW, layoutH);
        }
    }

    // --- PASS 2: FILL ---
    for (std::size_t i = 0; i < visibleGlyphCount; ++i) {
        const FontManager::PositionedGlyph& cg =
            cachedLayout_->glyphs[i];
        SDL_Texture* fillTexture = cg.dynamicAtlas
            ? mip->dynamicFillTexture
            : mip->fillTexture;
        if (fillTexture) {
            const float penStart =
                cg.advanceBefore * scale +
                cg.kerningBefore * kerningScale;

            SDL_FRect dst = {
                xOrigin + penStart - outline * scale,
                yOrigin + cg.packedY * scale,
                cg.srcFill.w * scale,
                cg.srcFill.h * scale
            };

            dst.x +=
                static_cast<float>(
                    cg.srcFill.x - cg.srcOutline.x
                ) * scale;

            dst.y +=
                static_cast<float>(
                    cg.srcFill.y - cg.srcOutline.y
                ) * scale;

            SDL::renderCopyF(fillTexture, baseViewInfo.Alpha, &cg.srcFill, &dst, baseViewInfo, layoutW, layoutH);
        }
    }

    SDL::endGeometryBatch();

    baseViewInfo.ImageWidth = oldIW;
    baseViewInfo.ImageHeight = oldIH;
}

bool Text::recycleAsText(const std::string& newText) {
    this->Component::freeGraphicsMemory();
    
    if (textData_ != newText) {
        textData_ = newText;
        needsUpdate_ = true;
        cachedLayout_.reset();
    }

    baseViewInfo.ImageWidth = 0;
    baseViewInfo.ImageHeight = 0;

    return true;
}
