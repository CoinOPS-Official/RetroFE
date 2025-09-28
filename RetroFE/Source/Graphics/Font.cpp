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
#include <deque>

 // Fill enclosed "holes" in an outline glyph, robust to antialiasing.
 // Any pixel with alpha <= thresh that is NOT connected to the border becomes opaque black.
// Size-aware hole close. Fills only "large" holes.
// - alphaThresh: pixels with alpha <= this are considered background.
// - minHoleArea: minimum area (in pixels) of a hole to be filled.
// - minHoleW/H : minimum width/height of a hole to be filled.
// Any of the size thresholds may be 0 to disable that check.
static void fillHolesInOutline(SDL_Surface* s,
    Uint8 alphaThresh = 16,
    int   minHoleArea = 0,
    int   minHoleW = 0,
    int   minHoleH = 0) {
    if (!s || s->format->BytesPerPixel != 4) return;

    SDL_LockSurface(s);
    const int w = s->w, h = s->h, pitch32 = s->pitch / 4;
    Uint32* px = (Uint32*)s->pixels;

    const Uint32 AMASK = s->format->Amask;
    const int    ASH = s->format->Ashift;

    auto aOf = [&](int x, int y)->Uint8 {
        return (Uint8)((px[y * pitch32 + x] & AMASK) >> ASH);
        };
    auto setOpaqueBlack = [&](int x, int y) {
        px[y * pitch32 + x] = 0xFF000000u; // ARGB
        };

    std::vector<uint8_t> ext(w * h, 0);   // external background marker
    std::vector<uint8_t> seen(w * h, 0);  // seen for component scans
    auto id = [w](int x, int y) { return y * w + x; };

    // 8-connected flood from border for alpha <= alphaThresh
    std::deque<std::pair<int, int>> q;
    auto pushIfZeroish = [&](int x, int y) {
        if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
        int k = id(x, y);
        if (ext[k]) return;
        if (aOf(x, y) <= alphaThresh) { ext[k] = 1; q.emplace_back(x, y); }
        };

    for (int x = 0; x < w; ++x) { pushIfZeroish(x, 0); pushIfZeroish(x, h - 1); }
    for (int y = 0; y < h; ++y) { pushIfZeroish(0, y); pushIfZeroish(w - 1, y); }

    const int dx8[8] = { +1,-1, 0, 0, +1,+1,-1,-1 };
    const int dy8[8] = { 0, 0,+1,-1, +1,-1,+1,-1 };

    while (!q.empty()) {
        auto [cx, cy] = q.front(); q.pop_front();
        for (int d = 0; d < 8; ++d) {
            int nx = cx + dx8[d], ny = cy + dy8[d];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) continue;
            int k = id(nx, ny);
            if (ext[k]) continue;
            if (aOf(nx, ny) <= alphaThresh) { ext[k] = 1; q.emplace_back(nx, ny); }
        }
    }

    // Scan for internal "hole" components (alpha <= thresh and !ext)
    std::vector<int> comp; comp.reserve(256);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int k0 = id(x, y);
            if (seen[k0]) continue;
            if (aOf(x, y) > alphaThresh) continue;
            if (ext[k0]) continue; // external background, skip

            // BFS collect a hole component
            comp.clear();
            int minx = x, maxx = x, miny = y, maxy = y;
            std::deque<std::pair<int, int>> qq;
            qq.emplace_back(x, y);
            seen[k0] = 1;

            while (!qq.empty()) {
                auto [cx, cy] = qq.front(); qq.pop_front();
                int kc = id(cx, cy);
                comp.push_back(kc);
                if (cx < minx) minx = cx; if (cx > maxx) maxx = cx;
                if (cy < miny) miny = cy; if (cy > maxy) maxy = cy;

                for (int d = 0; d < 8; ++d) {
                    int nx = cx + dx8[d], ny = cy + dy8[d];
                    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) continue;
                    int kn = id(nx, ny);
                    if (seen[kn]) continue;
                    if (ext[kn]) continue;
                    if (aOf(nx, ny) <= alphaThresh) {
                        seen[kn] = 1;
                        qq.emplace_back(nx, ny);
                    }
                }
            }

            // Decide whether to fill this component
            const int area = (int)comp.size();
            const int bw = maxx - minx + 1;
            const int bh = maxy - miny + 1;

            bool bigEnough = false;
            if (minHoleArea > 0 && area >= minHoleArea) bigEnough = true;
            if (minHoleW > 0 && bw >= minHoleW)    bigEnough = true;
            if (minHoleH > 0 && bh >= minHoleH)    bigEnough = true;

            if (bigEnough) {
                for (int kc : comp) {
                    int fy = kc / w, fx = kc % w;
                    setOpaqueBlack(fx, fy);
                }
            }
        }
    }

    SDL_UnlockSurface(s);
}


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
    if (texture_) { SDL_DestroyTexture(texture_);        texture_ = nullptr; }
    if (outlineTexture_) { SDL_DestroyTexture(outlineTexture_); outlineTexture_ = nullptr; }
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

    // Pixel masks
    Uint32 rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000; gmask = 0x00ff0000; bmask = 0x0000ff00; amask = 0x000000ff;
#else
    rmask = 0x000000ff; gmask = 0x0000ff00; bmask = 0x00ff0000; amask = 0xff000000;
#endif

    // Clear any previous
    clearAtlas();

    struct TmpGlyph {
        SDL_Surface* fill = nullptr; // ARGB8888 fill (gradiented if enabled)
        SDL_Surface* outline = nullptr; // ARGB8888 outline (black), nullptr when outlinePx_==0
        int dx = 0, dy = 0;            // center offsets of fill into outline box
        GlyphInfoBuild* info = nullptr;
        Uint16 ch = 0;
    };
    std::vector<TmpGlyph> tmp; tmp.reserve(128 - 32);

    // Build ASCII glyphs 32..127
    for (Uint16 ch = 32; ch < 128; ++ch) {
        int minx, maxx, miny, maxy, adv;
        if (TTF_GlyphMetrics(font_, ch, &minx, &maxx, &miny, &maxy, &adv) != 0) continue;

        // ---- Render FILL (white) ----
        TTF_SetFontOutline(font_, 0);
        SDL_Color white{ 255,255,255,255 };
        SDL_Surface* fill = TTF_RenderGlyph_Blended(font_, ch, white);
        if (!fill) continue;

        // Convert/gradient fill to ARGB8888
        if (gradient_) {
            fill = applyVerticalGrayGradient(fill, 255, 128);
            if (!fill) continue;
        }
        else if (fill->format->BytesPerPixel != 4) {
            SDL_Surface* conv = SDL_ConvertSurfaceFormat(fill, SDL_PIXELFORMAT_ARGB8888, 0);
            SDL_FreeSurface(fill);
            fill = conv;
            if (!fill) continue;
        }

        // ---- (Optional) Render OUTLINE (black) ----
        SDL_Surface* outline = nullptr;
        int dx = 0, dy = 0;

        if (outlinePx_ > 0) {
            TTF_SetFontOutline(font_, outlinePx_);
            outline = TTF_RenderGlyph_Blended(font_, ch, outlineColor_);
            TTF_SetFontOutline(font_, 0);

            if (outline) {
                if (outline->format->BytesPerPixel != 4) {
                    SDL_Surface* conv = SDL_ConvertSurfaceFormat(outline, SDL_PIXELFORMAT_ARGB8888, 0);
                    SDL_FreeSurface(outline);
                    outline = conv;
                    if (!outline) { SDL_FreeSurface(fill); continue; }
                }
                // After rendering/convert outline:
                const int px = outlinePx_;

                // Default: do NOT fill holes for very thin outlines
                int minArea = 0, minW = 0, minH = 0;

                // Only enable hole closing when outline is reasonably thick
                if (px >= 3) {
                    // Be a bit stricter so medium outlines don't overfill counters
                    // minArea ? 1.5 * px^2, minW/H ? px + 1
                    minArea = (px * px * 3) / 2;   // e.g., px=3 -> 13, px=4 -> 24
                    minW = px + 1;              // px=3 -> 4
                    minH = px + 1;              // px=3 -> 4
                }

                // You can also tighten alpha if needed (less likely to eat AA fringes)
                const Uint8 alphaThresh = 16; // try 12–20 if you want to tune

                fillHolesInOutline(outline, alphaThresh, minArea, minW, minH);
                
                dx = (outline->w - fill->w) / 2;
                dy = (outline->h - fill->h) / 2;
            }
        }

        // Packed box size = outline box if present, else fill box
        const int packedW = outline ? outline->w : fill->w;
        const int packedH = outline ? outline->h : fill->h;

        // Wrap row if needed
        if (x + packedW + GLYPH_SPACING > atlasWidth) {
            atlasHeight += y + GLYPH_SPACING;
            x = 0; y = 0;
        }

        // Record glyph metrics + rect (shared by fill/outline atlases)
        auto* info = new GlyphInfoBuild;
        info->surface = nullptr; // not used in dual-atlas path
        info->glyph.advance = adv;
        info->glyph.minX = minx; info->glyph.maxX = maxx;
        info->glyph.minY = miny; info->glyph.maxY = maxy;
        info->glyph.rect = { x, atlasHeight, packedW, packedH };

        // >>> NEW: record fill sub-rect relative to packed rect <<<
        if (outline) {
            info->glyph.fillX = dx;
            info->glyph.fillY = dy;
            info->glyph.fillW = fill->w;
            info->glyph.fillH = fill->h;
        }
        else {
            info->glyph.fillX = 0;
            info->glyph.fillY = 0;
            info->glyph.fillW = packedW;
            info->glyph.fillH = packedH;
        }

        atlas_[ch] = info;

        tmp.push_back(TmpGlyph{ fill, outline, dx, dy, info, ch });

        x += packedW + GLYPH_SPACING;
        y = std::max(y, packedH);
    }


    // Final atlas size
    atlasWidth = std::max(atlasWidth, x);
    atlasHeight += y + GLYPH_SPACING;

    // Create atlas surfaces
    SDL_Surface* atlasFill = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
    if (!atlasFill) {
        LOG_WARNING("Font", "Failed to create fill atlas surface.");
        TTF_CloseFont(font_); font_ = nullptr; clearAtlas(); return false;
    }
    SDL_FillRect(atlasFill, nullptr, SDL_MapRGBA(atlasFill->format, 0, 0, 0, 0));

    SDL_Surface* atlasOutline = nullptr;
    if (outlinePx_ > 0) {
        atlasOutline = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
        if (!atlasOutline) {
            LOG_WARNING("Font", "Failed to create outline atlas surface.");
            SDL_FreeSurface(atlasFill);
            TTF_CloseFont(font_); font_ = nullptr; clearAtlas(); return false;
        }
        SDL_FillRect(atlasOutline, nullptr, SDL_MapRGBA(atlasOutline->format, 0, 0, 0, 0));
    }

    // Blit glyphs into atlases
    for (const auto& t : tmp) {
        SDL_Rect dst = t.info->glyph.rect;

        if (atlasOutline && t.outline) {
            SDL_SetSurfaceBlendMode(t.outline, SDL_BLENDMODE_BLEND);
            SDL_BlitSurface(t.outline, nullptr, atlasOutline, &dst);
        }

        if (t.fill) {
            SDL_SetSurfaceBlendMode(t.fill, SDL_BLENDMODE_BLEND);
            SDL_Rect dstFill{ dst.x + t.dx, dst.y + t.dy, t.fill->w, t.fill->h };
            SDL_BlitSurface(t.fill, nullptr, atlasFill, &dstFill);
        }

        if (t.fill)    SDL_FreeSurface(t.fill);
        if (t.outline) SDL_FreeSurface(t.outline);
    }

    // Upload textures
    SDL_Texture* fillTex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasFill);
    if (!fillTex) {
        LOG_WARNING("Font", "Failed to create fill texture from surface.");
        SDL_FreeSurface(atlasFill);
        if (atlasOutline) SDL_FreeSurface(atlasOutline);
        TTF_CloseFont(font_); font_ = nullptr; clearAtlas(); return false;
    }
    SDL_SetTextureBlendMode(fillTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(fillTex, color_.r, color_.g, color_.b); // tint fill ONLY
    texture_ = fillTex;

    if (atlasOutline) {
        SDL_Texture* outTex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasOutline);
        if (!outTex) {
            LOG_WARNING("Font", "Failed to create outline texture from surface.");
            SDL_DestroyTexture(texture_); texture_ = nullptr;
            SDL_FreeSurface(atlasFill);
            SDL_FreeSurface(atlasOutline);
            TTF_CloseFont(font_); font_ = nullptr; clearAtlas(); return false;
        }
        SDL_SetTextureBlendMode(outTex, SDL_BLENDMODE_BLEND);
        outlineTexture_ = outTex;
    }
    else {
        // Ensure old outline texture (if any) is cleared
        if (outlineTexture_) { SDL_DestroyTexture(outlineTexture_); outlineTexture_ = nullptr; }
    }

    atlasW_ = atlasFill->w;
    atlasH_ = atlasFill->h;
    SDL_FreeSurface(atlasFill);
    if (atlasOutline) SDL_FreeSurface(atlasOutline);

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

int FontManager::getOutlinePx() const {
    return outlinePx_;
}