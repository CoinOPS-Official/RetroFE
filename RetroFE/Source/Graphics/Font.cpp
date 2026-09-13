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
#include <cmath>
#include <cstdint>
#include <utility>
#include <deque>
#include <cstring>
#include <memory>
#include <limits>

static constexpr int DYNAMIC_ATLAS_SIZE = 2048;

// This is a temporary struct used only during atlas generation for a single mip level.
struct GlyphInfoBuild {
    FontManager::GlyphInfo glyph;
    SDL_Surface* surface = nullptr; // Kept for compatibility with original code structure, but not used.
};

void FontManager::fillHolesInOutline(SDL_Surface* s,
    int alphaThresh,
    int minHoleArea,
    int minHoleW,
    int minHoleH) {
    if (!s || SDL_GetPixelFormatDetails(s->format)->bytes_per_pixel != 4) return;

    if (!SDL_LockSurface(s)) return;
    const int w = s->w, h = s->h, pitch32 = s->pitch / 4;
    Uint32* px = (Uint32*)s->pixels;

    const Uint32 AMASK = SDL_GetPixelFormatDetails(s->format)->Amask;
    const int    ASH = SDL_GetPixelFormatDetails(s->format)->Ashift;

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
                if (cx < minx) minx = cx;
                if (cx > maxx) maxx = cx;
                if (cy < miny) miny = cy;
                if (cy > maxy) maxy = cy;

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

// Helper: count rows of mostly-transparent pixels at the top of a glyph surface.
// This compensates for fonts that render punctuation with extra blank padding.
static int computeGlyphTopPad(SDL_Surface* s, Uint8 alphaThresh = 8) {
    if (!s || SDL_GetPixelFormatDetails(s->format)->bytes_per_pixel != 4) return 0;

    if (!SDL_LockSurface(s)) return 0;
    const int w = s->w;
    const int h = s->h;
    const int pitch32 = s->pitch / 4;
    Uint32* px = static_cast<Uint32*>(s->pixels);

    const Uint32 AMASK = SDL_GetPixelFormatDetails(s->format)->Amask;
    const int ASH = SDL_GetPixelFormatDetails(s->format)->Ashift;

    for (int y = 0; y < h; ++y) {
        Uint32* row = px + y * pitch32;
        for (int x = 0; x < w; ++x) {
            Uint8 a = static_cast<Uint8>((row[x] & AMASK) >> ASH);
            if (a > alphaThresh) {
                SDL_UnlockSurface(s);
                return y;
            }
        }
    }

    SDL_UnlockSurface(s);
    return 0;
}

// MipLevel destructor to release textures
FontManager::MipLevel::~MipLevel() {
    if (fillTexture)          SDL_DestroyTexture(fillTexture);
    if (outlineTexture)       SDL_DestroyTexture(outlineTexture);
    if (dynamicFillTexture)   SDL_DestroyTexture(dynamicFillTexture);
    if (dynamicOutlineTexture)SDL_DestroyTexture(dynamicOutlineTexture);
    if (font) { TTF_CloseFont(font); font = nullptr; }
}

// MODIFIED: Constructor now accepts maxFontSize
FontManager::FontManager(std::string fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor)
    : fontPath_(std::move(fontPath))
    , maxFontSize_(maxFontSize)
    , color_(color)
    , gradient_(gradient)
    , outlinePx_(outlinePx < 0 ? 0 : outlinePx)
    , monitor_(monitor) {
}

FontManager::~FontManager() { deInitialize(); }

SDL_Surface* FontManager::applyVerticalGrayGradient(SDL_Surface* s, Uint8 topGray, Uint8 bottomGray) {
    if (!s) return nullptr;
    if (s->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_Surface* conv = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(s);
        if (!conv) return nullptr;
        s = conv;
    }

    // sRGB <-> linear helpers (Rec. 709)
    auto toLinear = [](float u) -> float {
        return (u <= 0.04045f) ? (u / 12.92f) : std::pow((u + 0.055f) / 1.055f, 2.4f);
        };
    auto toSRGB = [](float u) -> float {
        return (u <= 0.0031308f) ? (12.92f * u) : (1.055f * std::pow(u, 1.0f / 2.4f) - 0.055f);
        };
    auto clamp01 = [](float v) -> float { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };

    if (!SDL_LockSurface(s)) { SDL_DestroySurface(s); return nullptr; }
    Uint8* px = static_cast<Uint8*>(s->pixels);
    const int pitch = s->pitch;

    for (int y = 0; y < s->h; ++y) {
        // vertical ramp in sRGB space (we?ll convert the result to linear)
        const float t = (s->h > 1) ? float(y) / float(s->h - 1) : 0.f;
        const float Gs = ((1.f - t) * (topGray / 255.0f)) + (t * (bottomGray / 255.0f));
        const float Glin = toLinear(Gs);  // target luminance in linear

        Uint32* row = reinterpret_cast<Uint32*>(px + y * pitch);
        for (int x = 0; x < s->w; ++x) {
            Uint8 r8, g8, b8, a8;
            SDL_GetRGBA(row[x], SDL_GetPixelFormatDetails(s->format), SDL_GetSurfacePalette(s), &r8, &g8, &b8, &a8);
            if (a8 == 0) continue;  // keep fully transparent untouched

            // current color in linear
            float r = toLinear(r8 / 255.0f);
            float g = toLinear(g8 / 255.0f);
            float b = toLinear(b8 / 255.0f);

            // current luminance (linear, Rec. 709)
            float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;

            // scale RGB to hit target luminance; if nearly black, just set to target gray
            if (Y > 1e-6f) {
                float m = Glin / Y;
                r *= m; g *= m; b *= m;
            }
            else {
                r = g = b = Glin;
            }

            // back to sRGB and pack (alpha preserved)
            Uint8 R = static_cast<Uint8>(std::round(clamp01(toSRGB(r)) * 255.0f));
            Uint8 G = static_cast<Uint8>(std::round(clamp01(toSRGB(g)) * 255.0f));
            Uint8 B = static_cast<Uint8>(std::round(clamp01(toSRGB(b)) * 255.0f));
            row[x] = SDL_MapSurfaceRGBA(s, R, G, B, a8);
        }
    }

    SDL_UnlockSurface(s);
    return s;
}

// NEW: Replaces clearAtlas, cleans up all mip levels
void FontManager::clearMips() {
    ++resourceGeneration_;
    max_font_ = nullptr;
    max_height_ = max_ascent_ = max_descent_ = 0;
    for (auto& kv : mipLevels_) delete kv.second;
    mipLevels_.clear();
}

void FontManager::preloadGlyphRange(TTF_Font* font, int outlinePx,
    Uint32 start, Uint32 end,
    int& x, int& y,
    int atlasWidth, int& atlasHeight,
    int GLYPH_SPACING,
    std::vector<TmpGlyph>& tmp,
    std::unordered_map<unsigned int, GlyphInfoBuild*>& temp_build) {
    for (Uint32 ch = start; ch <= end; ++ch) {
        if (!TTF_FontHasGlyph(font, ch)) continue;

        int minx, maxx, miny, maxy, adv;
        if (!TTF_GetGlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &adv)) continue;

        // Fill
        TTF_SetFontOutline(font, 0);
        SDL_Color white{ 255, 255, 255, 255 };
        SDL_Surface* fill = TTF_RenderGlyph_Blended(font, ch, white);
        if (!fill) continue;

        if (gradient_) {
            fill = applyVerticalGrayGradient(fill, 255, 128);
            if (!fill) continue;
        }
        else if (fill->format != SDL_PIXELFORMAT_ARGB8888) {
            SDL_Surface* conv = SDL_ConvertSurface(fill, SDL_PIXELFORMAT_ARGB8888);
            SDL_DestroySurface(fill);
            fill = conv;
            if (!fill) continue;
        }

        // Outline (optional)
        SDL_Surface* outline = nullptr;
        int dx = 0, dy = 0;
        if (outlinePx > 0) {
            TTF_SetFontOutline(font, outlinePx);
            outline = TTF_RenderGlyph_Blended(font, ch, outlineColor_);
            TTF_SetFontOutline(font, 0);
            if (outline) {
                if (outline->format != SDL_PIXELFORMAT_ARGB8888) {
                    SDL_Surface* conv = SDL_ConvertSurface(outline, SDL_PIXELFORMAT_ARGB8888);
                    SDL_DestroySurface(outline);
                    outline = conv;
                    if (!outline) { SDL_DestroySurface(fill); continue; }
                }
                const int px = outlinePx;
                int minArea = 0, minW = 0, minH = 0;
                if (px >= 3) {
                    minArea = (px * px * 3) / 2;
                    minW = px + 1;
                    minH = px + 1;
                }
                fillHolesInOutline(outline, 16, minArea, minW, minH);
                dx = (outline->w - fill->w) / 2;
                dy = (outline->h - fill->h) / 2;
            }
        }

        const int packedW = outline ? outline->w : fill->w;
        const int packedH = outline ? outline->h : fill->h;

        if (x + packedW + GLYPH_SPACING > atlasWidth) {
            atlasHeight += y + GLYPH_SPACING;
            x = 0;
            y = 0;
        }

        auto* info = new GlyphInfoBuild;
        info->glyph.advance = adv;
        info->glyph.minX = minx;
        info->glyph.maxX = maxx;
        info->glyph.minY = miny;
        info->glyph.maxY = maxy;
        info->glyph.rect = { x, atlasHeight, packedW, packedH };

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

        // Measure how much transparent padding exists at the top of the packed glyph surface.
        // This is used to stabilize baseline alignment for punctuation in some fonts.
        info->glyph.topPad = computeGlyphTopPad(outline ? outline : fill, 8);

        temp_build[ch] = info;
        tmp.push_back(TmpGlyph{ fill, outline, dx, dy, info });

        x += packedW + GLYPH_SPACING;
        y = std::max(y, packedH);
    }
}

bool FontManager::initialize() {
    clearMips();
    if (maxFontSize_ <= 0) return SDL_SetError("Invalid font size");
    preparedSizes_.insert(maxFontSize_);
    // Interleave full and three-quarter octaves: 96,72,48,36,24,18,12.
    constexpr int minimumSize = 12;
    for (double size = maxFontSize_; size >= minimumSize; size *= 0.5) {
        preparedSizes_.insert(static_cast<int>(std::lround(size)));
        if (size * 0.75 >= minimumSize)
            preparedSizes_.insert(static_cast<int>(std::lround(size * 0.75)));
    }
    if (maxFontSize_ > minimumSize) preparedSizes_.insert(minimumSize);
    for (int size : preparedSizes_) {
        if (!buildMip(size)) {
            clearMips();
            return false;
        }
    }
    return true;
}

bool FontManager::prepareSize(float size) {
    if (maxFontSize_ <= 0 || !std::isfinite(size) || size <= 0 || double(size) >= std::numeric_limits<int>::max())
        return SDL_SetError("Invalid prepared font size");
    const int rasterSize = static_cast<int>(std::ceil(size));
    if (!buildMip(rasterSize)) return false;
    preparedSizes_.insert(rasterSize);
    return true;
}

bool FontManager::prepareHeight(float height) {
    if (!max_font_ || !std::isfinite(height) || height <= 0 ||
        double(height) * maxFontSize_ / max_height_ >= std::numeric_limits<int>::max() - 1) return false;
    // Height-based consumers use the line box, not the point size. Prepare an
    // estimate and adjust for font hinting until its actual height covers it.
    int size = std::max(1, static_cast<int>(std::ceil(double(height) * maxFontSize_ / max_height_)));
    do {
        if (!prepareSize(static_cast<float>(size))) return false;
        if (mipLevels_.at(size)->height >= height) return true;
        ++size;
    } while (size < std::numeric_limits<int>::max());
    return false;
}

bool FontManager::buildMip(int currentSize) {
    if (mipLevels_.count(currentSize)) return true;
    // Preserve the outline's proportion to loadFontSize, with a minimum of
    // one raster pixel for outlined fonts. SDL_ttf uses integer outlines.
    const int outlinePx = outlinePx_ > 0
        ? std::max(1, static_cast<int>(std::lround(double(outlinePx_) * currentSize / maxFontSize_))) : 0;

    TTF_Font* font = TTF_OpenFont(fontPath_.c_str(), currentSize);
    if (!font) {
        LOG_WARNING("Font", "Failed to open font '" + fontPath_ + "' at size " +
            std::to_string(currentSize) + ": " + std::string(SDL_GetError()));
        return false;
    }

    TTF_SetFontKerning(font, 1);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);

    auto* mip = new MipLevel();
    mip->fontSize = currentSize;
    mip->outlinePx = outlinePx;
    mip->height = TTF_GetFontHeight(font);
    mip->ascent = TTF_GetFontAscent(font);
    mip->descent = TTF_GetFontDescent(font);
    mip->font = font; // Held per-mip handle

    const int GLYPH_SPACING = std::max(1, std::max(outlinePx + 1, currentSize / 16));
    int atlasWidth = std::min(1024, currentSize * 16);
    int atlasHeight = 0;
    int x = 0, y = 0;

    std::vector<TmpGlyph> tmp;
    tmp.reserve(128); // Sized for the standard alphanumeric range footprint
    std::unordered_map<unsigned int, GlyphInfoBuild*> temp_build;

    // --- OPTIMIZATION ---
    // Instead of rendering ASCII 32 through 1023 on startup, we only preload basic 
    // keyboard ASCII (32 to 126). Extended layout parameters, foreign characters, and symbols 
    // stream seamlessly into the streaming atlas dynamically via FontManager::loadGlyphOnDemand.
    preloadGlyphRange(font, outlinePx, 32, 126, x, y, atlasWidth, atlasHeight, GLYPH_SPACING, tmp, temp_build);

    atlasWidth = std::max(atlasWidth, x);
    atlasHeight += y + GLYPH_SPACING;

    SDL_Surface* atlasFill = SDL_CreateSurface(atlasWidth, atlasHeight, SDL_PIXELFORMAT_RGBA32);
    if (!atlasFill) {
        LOG_WARNING("Font", "Failed to create fill atlas surface for size " + std::to_string(currentSize));
        for (auto& glyph : tmp) { SDL_DestroySurface(glyph.fill); SDL_DestroySurface(glyph.outline); }
        for (auto& p : temp_build) delete p.second;
        delete mip;
        return false;
    }
    SDL_FillSurfaceRect(atlasFill, nullptr, SDL_MapSurfaceRGBA(atlasFill, 0, 0, 0, 0));

    SDL_Surface* atlasOutline = nullptr;
    if (outlinePx > 0) {
        atlasOutline = SDL_CreateSurface(atlasWidth, atlasHeight, SDL_PIXELFORMAT_RGBA32);
        if (!atlasOutline) {
            SDL_DestroySurface(atlasFill);
            for (auto& glyph : tmp) { SDL_DestroySurface(glyph.fill); SDL_DestroySurface(glyph.outline); }
            for (auto& p : temp_build) delete p.second;
            delete mip;
            return false;
        }
        SDL_FillSurfaceRect(atlasOutline, nullptr, SDL_MapSurfaceRGBA(atlasOutline, 0, 0, 0, 0));
    }

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
        if (t.fill)    SDL_DestroySurface(t.fill);
        if (t.outline) SDL_DestroySurface(t.outline);
    }

    // Create static atlas textures
    SDL_Texture* fillTex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasFill);
    if (fillTex) {
        SDL_SetTextureScaleMode(fillTex, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(fillTex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(fillTex, color_.r, color_.g, color_.b);
        mip->fillTexture = fillTex;
    }
    if (atlasOutline) {
        SDL_Texture* outTex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasOutline);
        if (outTex) {
            SDL_SetTextureScaleMode(outTex, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(outTex, SDL_BLENDMODE_BLEND);
            mip->outlineTexture = outTex;
        }
    }

    mip->atlasW = atlasFill->w;
    mip->atlasH = atlasFill->h;
    SDL_DestroySurface(atlasFill);
    if (atlasOutline) SDL_DestroySurface(atlasOutline);

    // Reserve Unicode space now, proportionate to raster size. Round upward
    // to a power of two so smaller glyphs retain ample packing capacity.
    mip->dynamicAtlasSize = 512;
    const double desiredAtlasSize = double(DYNAMIC_ATLAS_SIZE) * currentSize / maxFontSize_;
    while (mip->dynamicAtlasSize < DYNAMIC_ATLAS_SIZE && mip->dynamicAtlasSize < desiredAtlasSize)
        mip->dynamicAtlasSize *= 2;

    // Dynamic atlases - STREAMING
    mip->dynamicFillTexture = SDL_CreateTexture(
        SDL::getRenderer(monitor_),
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        mip->dynamicAtlasSize, mip->dynamicAtlasSize);
    if (mip->dynamicFillTexture) {
        SDL_SetTextureScaleMode(mip->dynamicFillTexture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(mip->dynamicFillTexture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(mip->dynamicFillTexture, color_.r, color_.g, color_.b);
        void* pixels = nullptr; int pitch = 0;
        SDL_Rect full{ 0,0,mip->dynamicAtlasSize,mip->dynamicAtlasSize };
        if (SDL_LockTexture(mip->dynamicFillTexture, &full, &pixels, &pitch)) {
            for (int y0 = 0; y0 < mip->dynamicAtlasSize; ++y0) {
                std::memset((Uint8*)pixels + y0 * pitch, 0x00, mip->dynamicAtlasSize * 4);
            }
            SDL_UnlockTexture(mip->dynamicFillTexture);
        }
    }
    else {
        LOG_WARNING("Font", "Failed to create dynamic fill texture");
    }

    if (outlinePx > 0) {
        mip->dynamicOutlineTexture = SDL_CreateTexture(
            SDL::getRenderer(monitor_),
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            mip->dynamicAtlasSize, mip->dynamicAtlasSize);
        if (mip->dynamicOutlineTexture) {
            SDL_SetTextureScaleMode(mip->dynamicOutlineTexture, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(mip->dynamicOutlineTexture, SDL_BLENDMODE_BLEND);
            void* pixels = nullptr; int pitch = 0;
            SDL_Rect full{ 0,0,mip->dynamicAtlasSize,mip->dynamicAtlasSize };
            if (SDL_LockTexture(mip->dynamicOutlineTexture, &full, &pixels, &pitch)) {
                for (int y0 = 0; y0 < mip->dynamicAtlasSize; ++y0) {
                    std::memset((Uint8*)pixels + y0 * pitch, 0x00, mip->dynamicAtlasSize * 4);
                }
                SDL_UnlockTexture(mip->dynamicOutlineTexture);
            }
        }
        else {
            LOG_WARNING("Font", "Failed to create dynamic outline texture");
        }
    }

    mip->dynamicNextX = 0;
    mip->dynamicNextY = 0;
    mip->dynamicRowHeight = 0;

    for (auto const& [key, val] : temp_build) {
        mip->glyphs[key] = val->glyph;
        delete val;
    }
    temp_build.clear();

    mipLevels_[currentSize] = mip;

    if (!mip->fillTexture || (outlinePx && !mip->outlineTexture) ||
        !mip->dynamicFillTexture || (outlinePx && !mip->dynamicOutlineTexture)) {
        mipLevels_.erase(currentSize);
        delete mip;
        return false;
    }
    if (currentSize == maxFontSize_) {
        max_font_ = mip->font;
        max_height_ = mip->height;
        max_ascent_ = mip->ascent;
        max_descent_ = mip->descent;
    }
    return true;
}

bool FontManager::loadGlyphOnDemand(Uint32 ch, MipLevel* mip) {
    if (!mip) return false;

    if (mip->glyphs.find(ch) != mip->glyphs.end())        return true;
    if (mip->dynamicGlyphs.find(ch) != mip->dynamicGlyphs.end()) return true;

    if (!mip->dynamicFillTexture) {
        LOG_ERROR("Font", "Dynamic atlas not initialized");
        return false;
    }
    if (!mip->font) {
        LOG_ERROR("Font", "Per-mip TTF_Font not available");
        return false;
    }

    const int outlinePx = mip->outlinePx;
    TTF_Font* font = mip->font;
    if (!TTF_FontHasGlyph(font, ch)) return false;

    int minx, maxx, miny, maxy, adv;
    if (!TTF_GetGlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &adv)) {
        return false;
    }

    // Render fill to surface (ARGB8888)
    TTF_SetFontOutline(font, 0);
    SDL_Color white{ 255, 255, 255, 255 };
    SDL_Surface* fill = TTF_RenderGlyph_Blended(font, ch, white);
    if (!fill) return false;

    if (gradient_) {
        fill = applyVerticalGrayGradient(fill, 255, 128);
        if (!fill) return false;
    }
    else if (fill->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_Surface* conv = SDL_ConvertSurface(fill, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(fill);
        fill = conv;
        if (!fill) return false;
    }

    SDL_Surface* outline = nullptr;
    int dx = 0, dy = 0;
    if (outlinePx > 0) {
        TTF_SetFontOutline(font, outlinePx);
        outline = TTF_RenderGlyph_Blended(font, ch, outlineColor_);
        TTF_SetFontOutline(font, 0);
        if (outline) {
            if (outline->format != SDL_PIXELFORMAT_ARGB8888) {
                SDL_Surface* conv = SDL_ConvertSurface(outline, SDL_PIXELFORMAT_ARGB8888);
                SDL_DestroySurface(outline);
                outline = conv;
                if (!outline) { SDL_DestroySurface(fill); return false; }
            }
            const int px = outlinePx;
            int minArea = 0, minW = 0, minH = 0;
            if (px >= 3) {
                minArea = (px * px * 3) / 2;
                minW = px + 1;
                minH = px + 1;
            }
            fillHolesInOutline(outline, 16, minArea, minW, minH);
            dx = (outline->w - fill->w) / 2;
            dy = (outline->h - fill->h) / 2;
        }
    }

    const int packedW = outline ? outline->w : fill->w;
    const int packedH = outline ? outline->h : fill->h;
    const int GLYPH_SPACING = std::max(1, std::max(outlinePx + 1, mip->fontSize / 16));

    // Shelf wrap
    if (mip->dynamicNextX + packedW + GLYPH_SPACING > mip->dynamicAtlasSize) {
        mip->dynamicNextY += mip->dynamicRowHeight + GLYPH_SPACING;
        mip->dynamicNextX = 0;
        mip->dynamicRowHeight = 0;
    }
    if (mip->dynamicNextY + packedH + GLYPH_SPACING > mip->dynamicAtlasSize) {
        LOG_WARNING("Font", "Dynamic atlas full; cannot load glyph U+" + std::to_string(ch));
        SDL_DestroySurface(fill);
        if (outline) SDL_DestroySurface(outline);
        return false;
    }

    // Build final glyph info
    GlyphInfo glyph;
    glyph.advance = adv;
    glyph.minX = minx;
    glyph.maxX = maxx;
    glyph.minY = miny;
    glyph.maxY = maxy;
    glyph.rect = { mip->dynamicNextX, mip->dynamicNextY, packedW, packedH };

    if (outline) {
        glyph.fillX = dx; glyph.fillY = dy;
        glyph.fillW = fill->w; glyph.fillH = fill->h;
    }
    else {
        glyph.fillX = 0; glyph.fillY = 0;
        glyph.fillW = packedW; glyph.fillH = packedH;
    }

    // Measure how much transparent padding exists at the top of the packed glyph surface.
    glyph.topPad = computeGlyphTopPad(outline ? outline : fill, 8);

    // --- Streaming upload (no temp textures, no RT switches) ---
    // Upload OUTLINE first (if present)
    if (outline && mip->dynamicOutlineTexture) {
        SDL_Rect dst{ glyph.rect.x, glyph.rect.y, outline->w, outline->h };
        if (!SDL_UpdateTexture(mip->dynamicOutlineTexture, &dst, outline->pixels, outline->pitch)) {
            LOG_WARNING("Font", std::string("SDL_UpdateTexture outline failed: ") + SDL_GetError());
        }
    }

    // Upload FILL (always)
    {
        SDL_Rect dst{ glyph.rect.x + glyph.fillX, glyph.rect.y + glyph.fillY, glyph.fillW, glyph.fillH };
        if (!SDL_UpdateTexture(mip->dynamicFillTexture, &dst, fill->pixels, fill->pitch)) {
            LOG_WARNING("Font", std::string("SDL_UpdateTexture fill failed: ") + SDL_GetError());
        }
    }

    // Color mod persists on the texture object (already set in initialize/setColor)

    // Track glyph
    mip->dynamicGlyphs[ch] = glyph;

    // Advance shelf
    mip->dynamicNextX += packedW + GLYPH_SPACING;
    mip->dynamicRowHeight = std::max(mip->dynamicRowHeight, packedH);

    SDL_DestroySurface(fill);
    if (outline) SDL_DestroySurface(outline);
    return true;
}

void FontManager::deInitialize() {
    clearMips();
    max_font_ = nullptr;
}

// MODIFIED: Applies color to all mip levels
void FontManager::setColor(SDL_Color c) {
    color_ = c;
    for (const auto& pair : mipLevels_) {
        if (pair.second && pair.second->fillTexture) {
            SDL_SetTextureColorMod(pair.second->fillTexture, color_.r, color_.g, color_.b);
        }
    }
}

// NEW: Gets the best mip level for a target rendering size
// In Font.cpp

const FontManager::MipLevel* FontManager::getMipLevelForSize(float targetSize) const {
    if (mipLevels_.empty() || !std::isfinite(targetSize)) return nullptr;
    for (const auto& [size, mip] : mipLevels_) {
        if (size >= targetSize) return mip;
    }
    return mipLevels_.rbegin()->second;
}

const FontManager::MipLevel* FontManager::getMipLevelForHeight(float targetHeight) const {
    if (mipLevels_.empty() || !std::isfinite(targetHeight)) return nullptr;
    const MipLevel* best = nullptr;
    const MipLevel* largest = nullptr;
    for (const auto& [size, mip] : mipLevels_) {
        if (!largest || mip->height > largest->height) largest = mip;
        if (mip->height >= targetHeight && (!best || mip->height < best->height)) best = mip;
    }
    return best ? best : largest;
}

// MODIFIED: Uses the max-resolution font handle for best precision
int FontManager::getKerning(Uint32 prevChar, Uint32 curChar) const {  // ? was Uint16
    if (!max_font_ || prevChar == 0 || curChar == 0) return 0;
    int kerning = 0;
    return TTF_GetGlyphKerning(max_font_, prevChar, curChar, &kerning) ? kerning : 0;
}

int FontManager::getKerning(const MipLevel& mip, Uint32 prevChar, Uint32 curChar) const {
    if (!mip.font || !prevChar || !curChar) return 0;
    int kerning = 0;
    return TTF_GetGlyphKerning(mip.font, prevChar, curChar, &kerning) ? kerning : 0;
}

int FontManager::getWidth(const std::string& text) {
    const auto it = mipLevels_.find(maxFontSize_);
    return it == mipLevels_.end() ? 0 : getWidth(text, *it->second);
}

float FontManager::getWidthForHeight(const std::string& text, float height) const {
    const auto* mip = getMipLevelForHeight(height);
    return mip && mip->height > 0 ? getWidth(text, *mip) * height / mip->height : 0.0f;
}

int FontManager::getWidth(const std::string& text, const MipLevel& mip) const {
    if (!mip.font) return 0;

    int width = 0;
    Uint32 prev = 0;
    bool haveGlyph = false;
    const char* ptr = text.c_str();
    size_t remaining = text.size();
    while (remaining) {
        const Uint32 ch = SDL_StepUTF8(&ptr, &remaining);
        if (!ch) break;
        int advance = 0;
        // Measurement must not depend on which glyphs have been uploaded.
        if (ch < 32 || !TTF_FontHasGlyph(mip.font, ch) ||
            !TTF_GetGlyphMetrics(mip.font, ch, nullptr, nullptr, nullptr, nullptr, &advance)) {
            prev = 0;
            continue;
        }
        haveGlyph = true;
        width += getKerning(mip, prev, ch) + advance;
        prev = ch;
    }

    if (haveGlyph && mip.outlinePx > 0) width += 2 * mip.outlinePx;
    return width;
}

int FontManager::getOutlinePx() const {
    return outlinePx_;
}
