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

 // This is a temporary struct used only during atlas generation for a single mip level.
struct GlyphInfoBuild {
    FontManager::GlyphInfo glyph;
    SDL_Surface* surface = nullptr; // Kept for compatibility with original code structure, but not used.
};

// Static utility function (unchanged)
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

// NEW: MipLevel destructor to release textures
FontManager::MipLevel::~MipLevel() {
    if (fillTexture) {
        SDL_DestroyTexture(fillTexture);
    }
    if (outlineTexture) {
        SDL_DestroyTexture(outlineTexture);
    }
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

FontManager::~FontManager() {
    deInitialize();
}

// Static helper (unchanged)
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

// NEW: Replaces clearAtlas, cleans up all mip levels
void FontManager::clearMips() {
    for (auto& pair : mipLevels_) {
        delete pair.second; // This invokes MipLevel::~MipLevel()
    }
    mipLevels_.clear();
}

// HEAVILY MODIFIED: initialize() now generates a chain of atlases
bool FontManager::initialize() {
    clearMips();

    // --- Mipmapping knobs ---
    const int   MIN_FONT_SIZE = 8;
    const float DENSE_FACTOR = 0.85f; // step when sizes are large
    const float SPARSE_FACTOR = 0.60f; // step when sizes are small
    const int   SWITCH_THRESHOLD = 40;

    struct GlyphInfoBuild {
        GlyphInfo   glyph;
        SDL_Surface* surface = nullptr; // unused, kept for symmetry
    };

    for (int currentSize = maxFontSize_; currentSize >= MIN_FONT_SIZE; ) {
        TTF_Font* font = TTF_OpenFont(fontPath_.c_str(), currentSize);
        if (!font) {
            LOG_WARNING("Font", "Failed to open font '" + fontPath_ + "' at size " +
                std::to_string(currentSize) + ": " + std::string(TTF_GetError()));
            float factor = (currentSize > SWITCH_THRESHOLD) ? DENSE_FACTOR : SPARSE_FACTOR;
            int   nextSize = (int)std::round(currentSize * factor);
            if (nextSize >= currentSize) break;
            currentSize = nextSize;
            continue;
        }

        // Gentler hinting reduces y-snapping that murders thin horizontals like '-'
        TTF_SetFontKerning(font, 1);
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT); // <— key tweak :contentReference[oaicite:1]{index=1}

        // Build one mip level
        MipLevel* mip = new MipLevel();
        mip->fontSize = currentSize;
        mip->height = TTF_FontHeight(font);
        mip->ascent = TTF_FontAscent(font);
        mip->descent = TTF_FontDescent(font);

        const int GLYPH_SPACING = std::max(1, std::max(outlinePx_ + 1, currentSize / 16));
        int atlasWidth = std::min(1024, currentSize * 16);
        int atlasHeight = 0;
        int x = 0, y = 0;

        Uint32 rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        rmask = 0xff000000; gmask = 0x00ff0000; bmask = 0x0000ff00; amask = 0x000000ff;
#else
        rmask = 0x000000ff; gmask = 0x0000ff00; bmask = 0x00ff0000; amask = 0xff000000;
#endif

        struct TmpGlyph {
            SDL_Surface* fill = nullptr;
            SDL_Surface* outline = nullptr;
            int              dx = 0, dy = 0;
            GlyphInfoBuild* info = nullptr;
        };
        std::vector<TmpGlyph> tmp; tmp.reserve(128 - 32);
        std::unordered_map<unsigned int, GlyphInfoBuild*> temp_build;

        for (Uint16 ch = 32; ch < 128; ++ch) {
            int minx, maxx, miny, maxy, adv;
            if (TTF_GlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &adv) != 0) continue;

            // Render fill
            TTF_SetFontOutline(font, 0);
            SDL_Color white{ 255,255,255,255 };
            SDL_Surface* fill = TTF_RenderGlyph_Blended(font, ch, white);
            if (!fill) continue;

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

            // Optional outline
            SDL_Surface* outline = nullptr;
            int dx = 0, dy = 0;
            if (outlinePx_ > 0) {
                TTF_SetFontOutline(font, outlinePx_);
                outline = TTF_RenderGlyph_Blended(font, ch, outlineColor_);
                TTF_SetFontOutline(font, 0);
                if (outline) {
                    if (outline->format->BytesPerPixel != 4) {
                        SDL_Surface* conv = SDL_ConvertSurfaceFormat(outline, SDL_PIXELFORMAT_ARGB8888, 0);
                        SDL_FreeSurface(outline);
                        outline = conv;
                        if (!outline) { SDL_FreeSurface(fill); continue; }
                    }
                    const int px = outlinePx_;
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

            // --- SYNTHETIC VERTICAL METRICS ---
            // SDL_ttf metrics (miny/maxy) are relative to baseline and in *pixels* at this size.
            // Bitmap height may differ due to hinting. To keep layout consistent, force:
            //   newHeight = fill->h
            //   keep centerline = (maxy + miny)/2
            // so that "top = ascent - maxY" places the bitmap exactly where metrics expect.
            const float metricsCenter = 0.5f * float(maxy + miny);
            const float halfBitmapH = 0.5f * float(fill->h);
            // Choose integer new minY/maxY that preserve center and match bitmap height.
            int newMinY = int(std::floor(metricsCenter - halfBitmapH + 0.5f));
            int newMaxY = newMinY + fill->h;

            const int packedW = outline ? outline->w : fill->w;
            const int packedH = outline ? outline->h : fill->h;

            if (x + packedW + GLYPH_SPACING > atlasWidth) {
                atlasHeight += y + GLYPH_SPACING;
                x = 0; y = 0;
            }

            auto* info = new GlyphInfoBuild;
            info->surface = nullptr;
            info->glyph.advance = adv;
            info->glyph.minX = minx;
            info->glyph.maxX = maxx;
            // Use synthetic vertical metrics (fixes hyphen/underscore misplacement)
            info->glyph.minY = newMinY;
            info->glyph.maxY = newMaxY;
            info->glyph.rect = { x, atlasHeight, packedW, packedH };

            if (outline) {
                info->glyph.fillX = dx; info->glyph.fillY = dy;
                info->glyph.fillW = fill->w; info->glyph.fillH = fill->h;
            }
            else {
                info->glyph.fillX = 0;  info->glyph.fillY = 0;
                info->glyph.fillW = packedW; info->glyph.fillH = packedH;
            }

            temp_build[ch] = info;
            tmp.push_back(TmpGlyph{ fill, outline, dx, dy, info });

            x += packedW + GLYPH_SPACING;
            y = std::max(y, packedH);
        }

        atlasWidth = std::max(atlasWidth, x);
        atlasHeight += y + GLYPH_SPACING;

        SDL_Surface* atlasFill = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
        if (!atlasFill) {
            LOG_WARNING("Font", "Failed to create fill atlas surface for size " + std::to_string(currentSize));
            for (auto& p : temp_build) delete p.second;
            delete mip;
            TTF_CloseFont(font);
            float factor = (currentSize > SWITCH_THRESHOLD) ? DENSE_FACTOR : SPARSE_FACTOR;
            int   nextSize = (int)std::round(currentSize * factor);
            if (nextSize >= currentSize) break;
            currentSize = nextSize;
            continue;
        }
        SDL_FillRect(atlasFill, nullptr, SDL_MapRGBA(atlasFill->format, 0, 0, 0, 0));

        SDL_Surface* atlasOutline = nullptr;
        if (outlinePx_ > 0) {
            atlasOutline = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
            if (!atlasOutline) {
                LOG_WARNING("Font", "Failed to create outline atlas surface for size " + std::to_string(currentSize));
                SDL_FreeSurface(atlasFill);
                for (auto& p : temp_build) delete p.second;
                delete mip;
                TTF_CloseFont(font);
                float factor = (currentSize > SWITCH_THRESHOLD) ? DENSE_FACTOR : SPARSE_FACTOR;
                int   nextSize = (int)std::round(currentSize * factor);
                if (nextSize >= currentSize) break;
                currentSize = nextSize;
                continue;
            }
            SDL_FillRect(atlasOutline, nullptr, SDL_MapRGBA(atlasOutline->format, 0, 0, 0, 0));
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
            if (t.fill)    SDL_FreeSurface(t.fill);
            if (t.outline) SDL_FreeSurface(t.outline);
        }

        SDL_Texture* fillTex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasFill);
        if (fillTex) {
            SDL_SetTextureBlendMode(fillTex, SDL_BLENDMODE_BLEND);
            mip->fillTexture = fillTex;
        }

        if (atlasOutline) {
            SDL_Texture* outTex = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasOutline);
            if (outTex) {
                SDL_SetTextureBlendMode(outTex, SDL_BLENDMODE_BLEND);
                mip->outlineTexture = outTex;
            }
        }

        mip->atlasW = atlasFill->w;
        mip->atlasH = atlasFill->h;
        SDL_FreeSurface(atlasFill);
        if (atlasOutline) SDL_FreeSurface(atlasOutline);

        for (auto const& [key, val] : temp_build) {
            mip->glyphs[key] = val->glyph;
            delete val;
        }
        temp_build.clear();

        mipLevels_[currentSize] = mip;

        if (currentSize == maxFontSize_) {
            // Keep the largest-size font handle open for precise kerning/metrics:contentReference[oaicite:2]{index=2}:contentReference[oaicite:3]{index=3}
            max_font_ = font;
            max_height_ = mip->height;
            max_ascent_ = mip->ascent;
            max_descent_ = mip->descent;
        }
        else {
            TTF_CloseFont(font);
        }

        float factor = (currentSize > SWITCH_THRESHOLD) ? DENSE_FACTOR : SPARSE_FACTOR;
        int   nextSize = (int)std::round(currentSize * factor);
        if (nextSize >= currentSize) break;
        currentSize = nextSize;
    }

    if (mipLevels_.empty()) {
        LOG_WARNING("Font", "Failed to generate any mip levels for font: " + fontPath_);
        if (max_font_) { TTF_CloseFont(max_font_); max_font_ = nullptr; }
        return false;
    }

    setColor(color_); // color-mod all mip textures:contentReference[oaicite:4]{index=4}
    return true;
}

void FontManager::deInitialize() {
    clearMips();
    if (max_font_) {
        TTF_CloseFont(max_font_);
        max_font_ = nullptr;
    }
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

const FontManager::MipLevel* FontManager::getMipLevelForSize(int targetSize) const {
    if (mipLevels_.empty()) return nullptr;

    // Find the first mip that is >= the target size.
    auto itCeil = mipLevels_.lower_bound(targetSize);

    const FontManager::MipLevel* best = nullptr;

    // Case 1: Target size is smaller than all available mips.
    // We must downscale from the smallest available mip.
    if (itCeil == mipLevels_.begin()) {
        best = itCeil->second;
    }
    // Case 2: Target size is larger than all available mips.
    // We must upscale from the largest available mip.
    else if (itCeil == mipLevels_.end()) {
        best = std::prev(itCeil)->second;
    }
    // Case 3: We are between two mips. itCeil is >= target, and itFloor is < target.
    else {
        auto itFloor = std::prev(itCeil);

        // --- The "Best of Both Worlds" logic ---
        const float UPSCALE_TOLERANCE_PERCENT = 0.15f; // Allow up to 15% upscaling
        int floorSize = itFloor->first;
        if ((targetSize - floorSize) <= (floorSize * UPSCALE_TOLERANCE_PERCENT)) {
            // Upscaling is tolerable, so choose the mip with the nearest size.
            int dUp = std::abs(itCeil->first - targetSize);
            int dDown = std::abs(floorSize - targetSize);

            // In case of a tie, prefer the larger mip (downscaling).
            best = (dDown < dUp) ? itFloor->second : itCeil->second;
        }
        else {
            // Upscaling is not tolerable, so we must downscale from the larger mip.
            best = itCeil->second;
        }
    }

    // --- ADD THIS DEBUG LOG ---
    // This will only compile and run in debug builds if LOG_DEBUG is enabled.
    //LOG_DEBUG("FontManager", "For target size " + std::to_string(targetSize) + ", picked atlas size " + std::to_string(best ? best->fontSize : 0));
    // -------------------------

    return best;
}// MODIFIED: Uses the max-resolution font handle for best precision
int FontManager::getKerning(Uint16 prevChar, Uint16 curChar) const {
    if (!max_font_ || prevChar == 0 || curChar == 0) return 0;
    return TTF_GetFontKerningSizeGlyphs(max_font_, prevChar, curChar);
}

// MODIFIED: Calculates width based on the metrics of the highest-resolution font
int FontManager::getWidth(const std::string& text) {
    if (mipLevels_.empty() || !max_font_) {
        return 0;
    }

    // Get glyph data from the highest-resolution mip level for measurement
    const MipLevel* maxMip = mipLevels_.rbegin()->second;

    int width = 0;
    Uint16 prev = 0;
    bool haveGlyph = false;

    for (unsigned char uc : text) {
        Uint16 ch = (Uint16)uc;
        auto it = maxMip->glyphs.find(ch);
        if (it != maxMip->glyphs.end()) {
            const GlyphInfo& g = it->second;
            haveGlyph = true;
            width += getKerning(prev, ch);
            width += g.advance;
            prev = ch;
        }
    }

    if (haveGlyph && outlinePx_ > 0) {
        width += 2 * outlinePx_;
    }
    return width;
}

int FontManager::getOutlinePx() const {
    return outlinePx_;
}