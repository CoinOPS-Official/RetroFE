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
#pragma once

#include <SDL2/SDL.h>
#if __has_include(<SDL2/SDL_ttf.h>)
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif

#include <string>
#include <unordered_map>

class FontManager {
public:
    struct GlyphInfo {
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        int advance = 0;

        // Packed rect in atlas (outline box if outlinePx_ > 0)
        SDL_Rect rect{ 0,0,0,0 };

        // NEW: fill sub-rect relative to 'rect' (so src = rect + {fillX,fillY,fillW,fillH})
        // When no outline is baked, these will be {0,0,rect.w,rect.h}.
        int fillX = 0;
        int fillY = 0;
        int fillW = 0;
        int fillH = 0;
    };

    FontManager(std::string fontPath, int fontSize, SDL_Color color, bool gradient, int outlinePx, int monitor);
    ~FontManager();

    // Lifetime
    bool initialize();
    void deInitialize();

    // Styling knobs
    void setGradientEnabled(bool on) { gradient_ = on; }
    void setOutline(int px, SDL_Color color) { outlinePx_ = (px < 0 ? 0 : px); outlineColor_ = color; }
    void setColor(SDL_Color c);   // updates SDL_SetTextureColorMod

    // Queries
    SDL_Texture* getTexture() { return texture_; }
    SDL_Texture* getOutlineTexture() const { return outlineTexture_; }
    bool getRect(unsigned int charCode, GlyphInfo& glyph);
    int  getHeight()   const { return height_; }
    int  getDescent()  const { return descent_; }
    int  getAscent()   const { return ascent_; }
    int  getFontSize() const { return fontSize_; }
    int  getAtlasW()   const { return atlasW_; }
    int  getAtlasH()   const { return atlasH_; }
    SDL_Color getColor() const { return color_; }

    // Metrics
    int  getKerning(Uint16 prevChar, Uint16 curChar) const;
    int  getWidth(const std::string& text);

    int getOutlinePx() const;

private:
    struct GlyphInfoBuild {
        GlyphInfo glyph;
        SDL_Surface* surface = nullptr;   // temp surface before upload
    };

    // Config
    std::string fontPath_;
    int fontSize_ = 0;
    SDL_Color color_{ 255,255,255,255 };
    int monitor_ = 0;

    bool gradient_;                // bake gray gradient (dark->light) into atlas
    int  outlinePx_;                  // 0 = no outline
    SDL_Color outlineColor_{ 0,0,0,255 };   // outline color

    // Runtime
    TTF_Font* font_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* outlineTexture_ = nullptr; // NEW: outline-only atlas
    int height_ = 0, descent_ = 0, ascent_ = 0;
    int atlasW_ = 0, atlasH_ = 0;

    std::unordered_map<unsigned int, GlyphInfoBuild*> atlas_; // ASCII map

    // Internal helpers
    static SDL_Surface* applyVerticalGrayGradient(SDL_Surface* s, Uint8 topGray = 255, Uint8 bottomGray = 64);
    void clearAtlas();
};
