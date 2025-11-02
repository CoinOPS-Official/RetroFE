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

    // Select the best MipLevel for the current render size
    const int targetFontSize = static_cast<int>(baseViewInfo.FontSize);
    const FontManager::MipLevel* mip = font->getMipLevelForSize(targetFontSize);
    if (!mip || !mip->fillTexture) {
        return;
    }

    if (mip) {
        LOG_INFO("Text", "Text: '" + textData_ + "' targetSize=" +
            std::to_string(targetFontSize) +
            " selectedMip=" + std::to_string(mip->fontSize) +
            " scale=" + std::to_string((mip->fontSize > 0) ? (baseViewInfo.FontSize / (float)mip->fontSize) : 1.f));  // ? Use same formula
    }

    // Get textures from the selected mip level
    SDL_Texture* fillTex = mip->fillTexture;
    SDL_Texture* outlineTex = mip->outlineTexture;

    // Calculate scale relative to the chosen mip's height
    const float scale = (mip->fontSize > 0) ? (baseViewInfo.FontSize / (float)mip->fontSize) : 1.f;

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

    // Compute origin
    const float oldW = baseViewInfo.Width;
    const float oldH = baseViewInfo.Height;
    const float oldIW = baseViewInfo.ImageWidth;
    const float oldIH = baseViewInfo.ImageHeight;

    baseViewInfo.Width = cachedWidth_;
    baseViewInfo.Height = baseViewInfo.FontSize;
    baseViewInfo.ImageWidth = float(mip->atlasW);
    baseViewInfo.ImageHeight = float(mip->atlasH);

    const float xOrigin = baseViewInfo.XRelativeToOrigin();
    const float yOrigin = baseViewInfo.YRelativeToOrigin();

    baseViewInfo.Width = oldW;
    baseViewInfo.Height = oldH;

    const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
    const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);

    // --- PASS 1: OUTLINE ---
    if (outlineTex || mip->dynamicOutlineTexture) {
        SDL_FRect dst;
        const char* ptr = textData_.c_str();
        const char* end = ptr + textData_.size();
        size_t i = 0;

        while (ptr < end && i < cachedPositions_.size()) {
            const auto& pos = cachedPositions_[i];

            // UTF-8 decode
            uint32_t codepoint = 0;
            unsigned char c = *ptr++;
            if (c < 0x80) {
                codepoint = c;
            }
            else if ((c & 0xE0) == 0xC0) {
                codepoint = ((c & 0x1F) << 6) | (*ptr++ & 0x3F);
            }
            else if ((c & 0xF0) == 0xE0) {
                codepoint = ((c & 0x0F) << 12) | ((*ptr++ & 0x3F) << 6) | (*ptr++ & 0x3F);
            }
            else if ((c & 0xF8) == 0xF0) {
                codepoint = ((c & 0x07) << 18) | ((*ptr++ & 0x3F) << 12) |
                    ((*ptr++ & 0x3F) << 6) | (*ptr++ & 0x3F);
            }
            else {
                continue; // Invalid UTF-8
            }

            Uint32 ch = codepoint;

            // Look up glyph (pre-loaded or dynamic)
            auto it = mip->glyphs.find(ch);
            bool isDynamic = false;
            if (it == mip->glyphs.end()) {
                it = mip->dynamicGlyphs.find(ch);
                isDynamic = true;
                if (it == mip->dynamicGlyphs.end()) {  // ? FIXED
                    i++;
                    continue;
                }
            }

            const FontManager::GlyphInfo& g = it->second;
            const SDL_Rect& srcOutline = g.rect;

            dst.x = xOrigin + pos.xOffset - g.fillX * scale;
            dst.y = yOrigin + pos.yOffset - g.fillY * scale;
            dst.w = srcOutline.w * scale;
            dst.h = srcOutline.h * scale;

            // Use correct texture (pre-loaded or dynamic)
            SDL_Texture* texToUse = isDynamic ? mip->dynamicOutlineTexture : outlineTex;
            if (texToUse) {
                SDL::renderCopyF(
                    texToUse,
                    baseViewInfo.Alpha,
                    &srcOutline,
                    &dst,
                    baseViewInfo,
                    layoutW, layoutH
                );
            }

            i++;
        }
    }

    // --- PASS 2: FILL ---
    {
        SDL_FRect dst;
        const char* ptr = textData_.c_str();
        const char* end = ptr + textData_.size();
        size_t i = 0;

        while (ptr < end && i < cachedPositions_.size()) {
            const auto& pos = cachedPositions_[i];

            // UTF-8 decode
            uint32_t codepoint = 0;
            unsigned char c = *ptr++;
            if (c < 0x80) {
                codepoint = c;
            }
            else if ((c & 0xE0) == 0xC0) {
                codepoint = ((c & 0x1F) << 6) | (*ptr++ & 0x3F);
            }
            else if ((c & 0xF0) == 0xE0) {
                codepoint = ((c & 0x0F) << 12) | ((*ptr++ & 0x3F) << 6) | (*ptr++ & 0x3F);
            }
            else if ((c & 0xF8) == 0xF0) {
                codepoint = ((c & 0x07) << 18) | ((*ptr++ & 0x3F) << 12) |
                    ((*ptr++ & 0x3F) << 6) | (*ptr++ & 0x3F);
            }
            else {
                continue; // Invalid UTF-8
            }

            Uint32 ch = codepoint;

            // Look up glyph (pre-loaded or dynamic)
            auto it = mip->glyphs.find(ch);
            bool isDynamic = false;
            if (it == mip->glyphs.end()) {
                it = mip->dynamicGlyphs.find(ch);
                isDynamic = true;
                if (it == mip->dynamicGlyphs.end()) {  // ? FIXED
                    i++;
                    continue;
                }
            }

            const FontManager::GlyphInfo& g = it->second;

            SDL_Rect srcFill{
                g.rect.x + g.fillX,
                g.rect.y + g.fillY,
                g.fillW,
                g.fillH
            };

            dst.x = xOrigin + pos.xOffset;
            dst.y = yOrigin + pos.yOffset;
            dst.w = g.fillW * scale;
            dst.h = g.fillH * scale;

            if (ch >= 65 && ch <= 90) {  // A-Z
                LOG_INFO("Text", "Rendering '" + std::string(1, (char)ch) +
                    "' dst.x=" + std::to_string(dst.x) +
                    " dst.w=" + std::to_string(dst.w) +
                    " src.w=" + std::to_string(srcFill.w));
            }

            // Use correct texture (pre-loaded or dynamic)
            SDL_Texture* texToUse = isDynamic ? mip->dynamicFillTexture : fillTex;

            if (ch >= 65 && ch <= 90) {
                LOG_INFO("Text", "Char '" + std::string(1, (char)ch) +
                    "' isDynamic=" + std::to_string(isDynamic) +
                    " texture=" + (texToUse == fillTex ? "PRELOADED" : "DYNAMIC"));
            }

            if (texToUse) {
                SDL::renderCopyF(
                    texToUse,
                    baseViewInfo.Alpha,
                    &srcFill,
                    &dst,
                    baseViewInfo,
                    layoutW, layoutH
                );
            }

            i++;
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

    double penX = 0.0;
    Uint32 prev = 0;

    struct PosTmp { SDL_Rect src; float xOff, yOff, advance_px; };
    std::vector<PosTmp> tmp; tmp.reserve(textData_.size());

    const char* ptr = textData_.c_str();
    const char* end = ptr + textData_.size();

    while (ptr < end) {
        uint32_t codepoint = 0;
        unsigned char c = *ptr++;

        if (c < 0x80) {
            codepoint = c;
        }
        else if ((c & 0xE0) == 0xC0) {
            codepoint = ((c & 0x1F) << 6) | (*ptr++ & 0x3F);
        }
        else if ((c & 0xF0) == 0xE0) {
            codepoint = ((c & 0x0F) << 12) | ((*ptr++ & 0x3F) << 6) | (*ptr++ & 0x3F);
        }
        else if ((c & 0xF8) == 0xF0) {
            codepoint = ((c & 0x07) << 18) | ((*ptr++ & 0x3F) << 12) |
                ((*ptr++ & 0x3F) << 6) | (*ptr++ & 0x3F);
        }
        else {
            prev = 0;
            continue;
        }

        const Uint32 ch = codepoint;

        auto it = mip->glyphs.find(ch);
        if (it == mip->glyphs.end() || it->second.rect.h <= 0) {
            it = mip->dynamicGlyphs.find(ch);
            if (it == mip->dynamicGlyphs.end()) {
                if (ch >= 1024) {
                    if (font->loadGlyphOnDemand(ch, const_cast<FontManager::MipLevel*>(mip))) {
                        it = mip->dynamicGlyphs.find(ch);
                        if (it == mip->dynamicGlyphs.end() || it->second.rect.h <= 0) {
                            prev = 0;
                            continue;
                        }
                    }
                    else {
                        prev = 0;
                        continue;
                    }
                }
                else {
                    prev = 0;
                    continue;
                }
            }
        }

        const auto& g = it->second;

        if (ch >= 65 && ch <= 90) {  // A-Z
            bool isDynamic = (mip->glyphs.find(ch) == mip->glyphs.end());
            LOG_INFO("Text", "Char '" + std::string(1, (char)ch) +
                "' advance=" + std::to_string(g.advance) +
                " minX=" + std::to_string(g.minX) +
                " maxX=" + std::to_string(g.maxX) +
                " source=" + (isDynamic ? "DYNAMIC" : "PRELOADED"));
        }

        // Get kerning and apply to pen
        const int kern_fp = font->getKerning(prev, ch);
        const float kern_px = static_cast<float>(kern_fp) * kerningScale;
        penX += static_cast<double>(kern_px);

        // FIXED: Position glyph at penX directly (don't add minX!)
        const float gx = static_cast<float>(penX);

        // Y position (this was already correct)
        const float minY_px = static_cast<float>(g.minY) * scale;
        const float gy = (ascent_f * scale) - minY_px - (g.fillH * scale);

        // Advance pen
        const float adv_px = static_cast<float>(g.advance) * scale;

        if (ch >= 65 && ch <= 90) {  // A-Z
            LOG_INFO("Text", "Char '" + std::string(1, (char)ch) +
                "' penX=" + std::to_string(penX) +
                " gx=" + std::to_string(gx) +
                " kern_px=" + std::to_string(kern_px) +
                " adv_px=" + std::to_string(adv_px));
        }

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