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
#define NOMINMAX
#include "Font.h"
#include "../SDL.h"
#include "../Utility/Log.h"

#include <algorithm>
#include <cstdint>
#include <utility>

 // -------------------- ctor / dtor --------------------

FontManager::FontManager(std::string fontPath, int fontSize, SDL_Color color, bool gradient, int outlinePx, int monitor)
    : fontPath_(std::move(fontPath))
    , fontSize_(fontSize)
    , color_(color)
	, gradient_(gradient)
	, outlinePx_(outlinePx < 0 ? 0 : outlinePx)
    , monitor_(monitor) {
}

FontManager::~FontManager() {
    deInitialize();
}

// -------------------- tiny helpers --------------------

SDL_Surface* FontManager::applyVerticalGrayGradient(SDL_Surface* s, Uint8 topGray, Uint8 bottomGray) {
    if (!s) return nullptr;
    if (s->format->BytesPerPixel != 4) {
        SDL_Surface* conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_FreeSurface(s);
        if (!conv) return nullptr;
        s = conv;
    }

    SDL_LockSurface(s);
    Uint8* px = static_cast<Uint8*>(s->pixels);
    const int pitch = s->pitch;

    for (int y = 0; y < s->h; ++y) {
        const float t = (s->h > 1) ? float(y) / float(s->h - 1) : 0.f;
        const Uint8 G = Uint8((1.f - t) * topGray + t * bottomGray);
        Uint32* row = reinterpret_cast<Uint32*>(px + y * pitch);
        for (int x = 0; x < s->w; ++x) {
            Uint8 r, g, b, a; SDL_GetRGBA(row[x], s->format, &r, &g, &b, &a);
            if (a) row[x] = SDL_MapRGBA(s->format, G, G, G, a); // recolor, keep alpha
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

void FontManager::clearAtlas() {
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
    for (auto& kv : atlas_) {
        if (kv.second) {
            if (kv.second->surface) SDL_FreeSurface(kv.second->surface);
            delete kv.second;
        }
    }
    atlas_.clear();
    atlasW_ = atlasH_ = 0;
}

// -------------------- public API --------------------

bool FontManager::initialize() {
    // Open font
    font_ = TTF_OpenFont(fontPath_.c_str(), fontSize_);
    if (!font_) {
        LOG_WARNING("Font", "Failed to open font: " + std::string(TTF_GetError()));
        return false;
    }
    TTF_SetFontKerning(font_, 1);
    TTF_SetFontHinting(font_, TTF_HINTING_NORMAL);

    // Metrics
    height_ = TTF_FontHeight(font_);
    ascent_ = TTF_FontAscent(font_);
    descent_ = TTF_FontDescent(font_);

    // Packing params (pad for outline if present)
    const int GLYPH_SPACING = std::max(1, std::max(outlinePx_ + 1, fontSize_ / 16));
    int atlasWidth = std::min(1024, fontSize_ * 16);
    int atlasHeight = 0;
    int x = 0, y = 0;

    // Pixel masks (only used when not creating with format)
    Uint32 rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000; gmask = 0x00ff0000; bmask = 0x0000ff00; amask = 0x000000ff;
#else
    rmask = 0x000000ff; gmask = 0x0000ff00; bmask = 0x00ff0000; amask = 0xff000000;
#endif

    // Clear any previous
    clearAtlas();

    // Build ASCII glyphs 32..127
    for (Uint16 ch = 32; ch < 128; ++ch) {
        int minx, maxx, miny, maxy, adv;
        if (TTF_GlyphMetrics(font_, ch, &minx, &maxx, &miny, &maxy, &adv) != 0) continue;

        // Render fill (white)
        TTF_SetFontOutline(font_, 0);
        SDL_Color white{ 255,255,255,255 };
        SDL_Surface* fill = TTF_RenderGlyph_Blended(font_, ch, white);
        if (!fill) continue;

        // Convert + optional gradient
        if (gradient_) {
            fill = applyVerticalGrayGradient(fill, /*top*/255, /*bottom*/128);
            if (!fill) continue;
        }
        else if (fill->format->BytesPerPixel != 4) {
            SDL_Surface* conv = SDL_ConvertSurfaceFormat(fill, SDL_PIXELFORMAT_ARGB8888, 0);
            SDL_FreeSurface(fill);
            fill = conv;
            if (!fill) continue;
        }

        SDL_Surface* finalSurf = fill;

        // Optional outline: render outline box and center the fill into it
        if (outlinePx_ > 0) {
            TTF_SetFontOutline(font_, outlinePx_);
            SDL_Surface* outline = TTF_RenderGlyph_Blended(font_, ch, outlineColor_);
            TTF_SetFontOutline(font_, 0);

            if (outline) {
                if (outline->format->BytesPerPixel != 4) {
                    SDL_Surface* conv = SDL_ConvertSurfaceFormat(outline, SDL_PIXELFORMAT_ARGB8888, 0);
                    SDL_FreeSurface(outline);
                    outline = conv;
                    if (!outline) { SDL_FreeSurface(fill); continue; }
                }

                SDL_Surface* target = SDL_CreateRGBSurfaceWithFormat(0, outline->w, outline->h, 32, outline->format->format);
                if (!target) { SDL_FreeSurface(outline); SDL_FreeSurface(fill); continue; }
                SDL_FillRect(target, nullptr, SDL_MapRGBA(target->format, 0, 0, 0, 0));

                // blit outline, then centered fill
                SDL_SetSurfaceBlendMode(outline, SDL_BLENDMODE_BLEND);
                SDL_Rect dO{ 0,0, outline->w, outline->h };
                SDL_BlitSurface(outline, nullptr, target, &dO);

                SDL_SetSurfaceBlendMode(fill, SDL_BLENDMODE_BLEND);
                const int dx = (outline->w - fill->w) / 2;
                const int dy = (outline->h - fill->h) / 2;
                SDL_Rect dF{ dx, dy, fill->w, fill->h };
                SDL_BlitSurface(fill, nullptr, target, &dF);

                SDL_FreeSurface(outline);
                SDL_FreeSurface(fill);
                finalSurf = target;
            }
        }

        // Wrap row if needed
        if (x + finalSurf->w + GLYPH_SPACING > atlasWidth) {
            atlasHeight += y + GLYPH_SPACING;
            x = 0; y = 0;
        }

        // Record glyph
        auto* info = new GlyphInfoBuild;
        info->surface = finalSurf;
        info->glyph.advance = adv;
        info->glyph.minX = minx; info->glyph.maxX = maxx;
        info->glyph.minY = miny; info->glyph.maxY = maxy;
        info->glyph.rect = { x, atlasHeight, finalSurf->w, finalSurf->h };
        atlas_[ch] = info;

        x += finalSurf->w + GLYPH_SPACING;
        y = std::max(y, finalSurf->h);
    }

    // Final atlas surface
    atlasWidth = std::max(atlasWidth, x);
    atlasHeight += y + GLYPH_SPACING;

    SDL_Surface* atlasSurface = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
    if (!atlasSurface) {
        LOG_WARNING("Font", "Failed to create atlas surface.");
        TTF_CloseFont(font_); font_ = nullptr;
        clearAtlas();
        return false;
    }
    SDL_FillRect(atlasSurface, nullptr, SDL_MapRGBA(atlasSurface->format, 0, 0, 0, 0));

    // Blit each glyph into atlas
    for (const auto& kv : atlas_) {
        GlyphInfoBuild* info = kv.second;
        if (!info || !info->surface) continue;
        SDL_SetSurfaceBlendMode(info->surface, SDL_BLENDMODE_NONE); // raw copy RGBA
        SDL_BlitSurface(info->surface, nullptr, atlasSurface, &info->glyph.rect);
        SDL_FreeSurface(info->surface);
        info->surface = nullptr;
    }

    // Upload to texture
    SDL_Texture* tex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasSurface);
    if (!tex) {
        LOG_WARNING("Font", "Failed to create texture from atlas surface.");
        SDL_FreeSurface(atlasSurface);
        TTF_CloseFont(font_); font_ = nullptr;
        clearAtlas();
        return false;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(tex, color_.r, color_.g, color_.b); // tint with user color
    texture_ = tex;

    atlasW_ = atlasSurface->w;
    atlasH_ = atlasSurface->h;
    SDL_FreeSurface(atlasSurface);

    return true;
}

void FontManager::deInitialize() {
    clearAtlas();
    if (font_) { TTF_CloseFont(font_); font_ = nullptr; }
}

void FontManager::setColor(SDL_Color c) {
    color_ = c;
    if (texture_) {
        SDL_SetTextureColorMod(texture_, color_.r, color_.g, color_.b);
    }
}

bool FontManager::getRect(unsigned int charCode, GlyphInfo& glyph) {
    auto it = atlas_.find(charCode);
    if (it != atlas_.end()) { glyph = it->second->glyph; return true; }
    return false;
}

int FontManager::getKerning(Uint16 prevChar, Uint16 curChar) const {
    if (!font_ || prevChar == 0 || curChar == 0) return 0;
    return TTF_GetFontKerningSizeGlyphs(font_, prevChar, curChar);
}

int FontManager::getWidth(const std::string& text) {
    int width = 0;
    Uint16 prev = 0;
    bool haveGlyph = false;

    for (unsigned char uc : text) {
        Uint16 ch = (Uint16)uc;
        GlyphInfo g;
        if (getRect(ch, g)) {
            haveGlyph = true;
            width += getKerning(prev, ch);
            width += g.advance;
            prev = ch;
        }
    }

    // Add visual pads only if there was at least one glyph and outline is on
    if (haveGlyph && outlinePx_ > 0) {
        width += 2 * outlinePx_;
    }
    return width;
}