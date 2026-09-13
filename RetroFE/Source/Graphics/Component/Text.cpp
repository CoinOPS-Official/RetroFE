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
#include "../GeometryBatch.h"
#include <cmath>
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

    const float effectiveFontSize = baseViewInfo.FontSize > 0
        ? baseViewInfo.FontSize
        : static_cast<float>(font->getMaxFontSize());
    const int targetFontSize = static_cast<int>(std::ceil(effectiveFontSize));
    const FontManager::MipLevel* mip = font->getMipLevelForSize(targetFontSize);
    if (!mip || !mip->fillTexture) return;

    const float scale = (mip->fontSize > 0)
        ? (effectiveFontSize / (float)mip->fontSize)
        : 1.f;

    const float maxW =
        (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    if (needsUpdate_ || lastFont_ != font || lastFontGeneration_ != font->getResourceGeneration() ||
        lastMipSize_ != mip->fontSize || lastScale_ != scale || lastMaxWidth_ != maxW) {
        updateGlyphPositions(font, scale, maxW);
        needsUpdate_ = false;
        lastScale_ = scale;
        lastMaxWidth_ = maxW;
        lastFont_ = font;
        lastFontGeneration_ = font->getResourceGeneration();
        lastMipSize_ = mip->fontSize;
    }
    if (cachedPositions_.empty()) return;

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

    // --- PASS 1: OUTLINE ---
    GeometryBatch batch;
    for (const auto& cg : cachedPositions_) {
        if (cg.outlineTex) {
            SDL_FRect dst = {
                xOrigin + cg.dstOutlineX,
                yOrigin + cg.dstOutlineY,
                cg.dstOutlineW,
                cg.dstOutlineH
            };
            SDL::appendCopyF(batch, cg.outlineTex, baseViewInfo.Alpha, &cg.srcOutline, &dst, baseViewInfo, layoutW, layoutH);
        }
    }

    // --- PASS 2: FILL ---
    for (const auto& cg : cachedPositions_) {
        if (cg.fillTex) {
            SDL_FRect dst = {
                xOrigin + cg.dstFillX,
                yOrigin + cg.dstFillY,
                cg.dstFillW,
                cg.dstFillH
            };
            SDL::appendCopyF(batch, cg.fillTex, baseViewInfo.Alpha, &cg.srcFill, &dst, baseViewInfo, layoutW, layoutH);
        }
    }

    batch.flush();
    baseViewInfo.ImageWidth = oldIW;
    baseViewInfo.ImageHeight = oldIH;
}

void Text::updateGlyphPositions(FontManager* font, float scale, float maxWidth) {
    cachedPositions_.clear();

    if (!font) return;

    cachedPositions_.reserve(textData_.size());

    const int targetFontSize = baseViewInfo.FontSize > 0
        ? static_cast<int>(std::ceil(baseViewInfo.FontSize))
        : font->getMaxFontSize();
    const FontManager::MipLevel* mip = font->getMipLevelForSize(targetFontSize);
    if (!mip) return;

    const float ascent_f = static_cast<float>(mip->ascent);
    const float outline_f = static_cast<float>(mip->outlinePx);

    double penX = 0.0;
    Uint32 prev = 0;

    const char* ptr = textData_.c_str();
    size_t remaining = textData_.size();

    while (remaining) {
        const Uint32 ch = SDL_StepUTF8(&ptr, &remaining);
        if (!ch) break;
        if (ch < 32) {
            prev = 0;
            continue;
        }

        bool isDynamic = false;

        auto it = mip->glyphs.find(ch);
        if (it == mip->glyphs.end() || it->second.rect.h <= 0) {

            it = mip->dynamicGlyphs.find(ch);
            isDynamic = true; // 2. Flag it the moment we touch the dynamic map

            if (it == mip->dynamicGlyphs.end()) {
                if (!font->loadGlyphOnDemand(ch, const_cast<FontManager::MipLevel*>(mip))) {
                    prev = 0;
                    continue;
                }
                it = mip->dynamicGlyphs.find(ch);
            }
            if (it == mip->dynamicGlyphs.end() || it->second.rect.h <= 0) {
                prev = 0;
                continue;
            }
        }

        const auto& g = it->second;

        const int   kern_fp = font->getKerning(*mip, prev, ch);
        const float kern_px = static_cast<float>(kern_fp) * scale;
        penX += static_cast<double>(kern_px);

        const float packedX = static_cast<float>(penX) - (outline_f * scale);
        const float packedY = (ascent_f - (static_cast<float>(g.maxY) + outline_f + static_cast<float>(g.topPad))) * scale;

        const float adv_px = static_cast<float>(g.advance) * scale;
        const double nextPen = penX + static_cast<double>(adv_px);

        if (maxWidth > 0.0f && static_cast<float>(nextPen) > maxWidth) break;

        // Build the fat cache entry
        CachedGlyph cg;
        cg.srcOutline = g.rect;
        cg.srcFill = { g.rect.x + g.fillX, g.rect.y + g.fillY, g.fillW, g.fillH };

        cg.dstOutlineX = packedX;
        cg.dstOutlineY = packedY;
        cg.dstOutlineW = g.rect.w * scale;
        cg.dstOutlineH = g.rect.h * scale;

        cg.dstFillX = packedX + (g.fillX * scale);
        cg.dstFillY = packedY + (g.fillY * scale);
        cg.dstFillW = g.fillW * scale;
        cg.dstFillH = g.fillH * scale;

        // 3. Use the boolean directly. No extra lookups!
        cg.outlineTex = isDynamic ? mip->dynamicOutlineTexture : mip->outlineTexture;
        cg.fillTex = isDynamic ? mip->dynamicFillTexture : mip->fillTexture;

        cachedPositions_.push_back(cg);

        penX = nextPen;
        prev = ch;
    }

    cachedWidth_ = static_cast<float>(penX);
    cachedHeight_ = static_cast<float>(mip->height) * scale + (2.0f * outline_f * scale);
}

bool Text::recycleAsText(const std::string& newText) {
    this->Component::freeGraphicsMemory();
    
    if (textData_ != newText) {
        textData_ = newText;
        needsUpdate_ = true;
    }

    baseViewInfo.ImageWidth = 0;
    baseViewInfo.ImageHeight = 0;

    return true;
}
