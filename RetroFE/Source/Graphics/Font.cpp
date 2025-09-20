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
#include "Font.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include <SDL2/SDL.h>
#if __has_include(<SDL2/SDL_ttf.h>)
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif
#include <cstdio>
#include <cstring>

FontManager::FontManager(std::string fontPath, int fontSize, SDL_Color color, int monitor)
    : texture(nullptr)
    , height (0)
    , descent (0)
    , ascent (0)
	, atlasW(0)
	, atlasH(0)
    , fontPath_(fontPath)
    , fontSize_(fontSize)
    , color_(color)
    , monitor_(monitor)
	, font_(nullptr)
{
}

FontManager::~FontManager()
{
    deInitialize();
}

SDL_Texture* FontManager::getTexture()
{
    return texture;
}

int FontManager::getHeight() const
{
    return height;
}

int FontManager::getWidth(const std::string& text) {
    int width = 0;
    Uint16 prevChar = 0;

    for (unsigned char uc : text) {
        Uint16 ch = (Uint16)uc;
        GlyphInfo glyph;
        if (getRect(ch, glyph)) {
            width += getKerning(prevChar, ch);  // use helper
            width += glyph.advance;
            prevChar = ch;
        }
    }
    return width;
}

int FontManager::getKerning(Uint16 prevChar, Uint16 curChar) const {
    if (!font_ || prevChar == 0 || curChar == 0) return 0;
    return TTF_GetFontKerningSizeGlyphs(font_, prevChar, curChar);
}

int  FontManager::getAtlasW() const 
{ 
    return atlasW; 
}

int  FontManager::getAtlasH() const 
{ 
    return atlasH; 
}

int FontManager::getFontSize() const
{
    return fontSize_;
}

int FontManager::getDescent() const
{
    return descent;
}

int FontManager::getAscent() const
{
    return ascent;
}

SDL_Color FontManager::getColor() const
{
    return color_;
}

bool FontManager::getRect(unsigned int charCode, GlyphInfo& glyph) {
    auto it = atlas.find(charCode); // Iterator type automatically adjusted

    if (it != atlas.end()) {
        GlyphInfoBuild const* info = it->second;
        glyph = info->glyph;
        return true;
    }
    return false;
}

bool FontManager::initialize() {
    font_ = TTF_OpenFont(fontPath_.c_str(), fontSize_);
    if (!font_) {
        LOG_WARNING("Font", "Failed to open font: " + std::string(TTF_GetError()));
        return false;
    }
    TTF_SetFontKerning(font_, 1);  // enable kerning
    TTF_SetFontHinting(font_, TTF_HINTING_NORMAL);
    const int GLYPH_SPACING = fontSize_ / 16; // Scale spacing with font size

    int x = 0, y = 0;
    int atlasHeight = 0;
    int atlasWidth = std::min(1024, fontSize_ * 16); // Dynamic width with max limit

    height = TTF_FontHeight(font_);
    ascent = TTF_FontAscent(font_);
    descent = TTF_FontDescent(font_); // Capture descent

    for (Uint16 i = 32; i < 128; ++i) {
        GlyphInfoBuild* info = new GlyphInfoBuild;

        color_.a = 255;
        info->surface = TTF_RenderGlyph_Blended(font_, i, color_);
        if (!info->surface) {
            delete info;
            continue;
        }
        SDL_SetSurfaceBlendMode(info->surface, SDL_BLENDMODE_NONE);

        TTF_GlyphMetrics(font_, i, &info->glyph.minX, &info->glyph.maxX, &info->glyph.minY, &info->glyph.maxY, &info->glyph.advance);

        // Check width limit and wrap to a new row if needed
        if (x + info->surface->w + GLYPH_SPACING > atlasWidth) { // Add spacing to width check
            atlasHeight += y + GLYPH_SPACING; // Add spacing to row height
            x = 0;
            y = 0;
        }

        // Adjust glyph rectangle to include spacing
        info->glyph.rect = { x, atlasHeight, info->surface->w, info->surface->h };
        atlas[i] = info;

        x += info->glyph.rect.w + GLYPH_SPACING; // Add spacing to x position
        y = std::max(y, info->glyph.rect.h);
    }

    atlasWidth = std::max(atlasWidth, x); // Final atlas width
    atlasHeight += y + GLYPH_SPACING;    // Final atlas height, including bottom spacing

    // Define masks based on byte order
    unsigned int rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000;
    gmask = 0x00ff0000;
    bmask = 0x0000ff00;
    amask = 0x000000ff;
#else
    rmask = 0x000000ff;
    gmask = 0x0000ff00;
    bmask = 0x00ff0000;
    amask = 0xff000000;
#endif

    // Create the atlas surface with the appropriate color masks
    SDL_Surface* atlasSurface = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
    if (!atlasSurface) {
        LOG_WARNING("Font", "Failed to create atlas surface.");
        TTF_CloseFont(font_);
        return false;
    }

    // Blit each glyph onto the atlas surface
    for (const auto& pair : atlas) {
        GlyphInfoBuild* info = pair.second;
        SDL_SetSurfaceBlendMode(info->surface, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(info->surface, nullptr, atlasSurface, &info->glyph.rect);
        SDL_FreeSurface(info->surface);
        info->surface = nullptr;
    }

    texture = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasSurface);

    if (!texture) {
        LOG_WARNING("Font", "Failed to create texture from surface.");
        SDL_FreeSurface(atlasSurface);
        TTF_CloseFont(font_);
        return false;
    }

    atlasW = atlasSurface->w;
    atlasH = atlasSurface->h;

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(atlasSurface);

    return true;
}

void FontManager::deInitialize() {
    if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
    for (auto& pair : atlas) delete pair.second;
    atlas.clear();
    atlasW = atlasH = 0;

    if (font_) {                 // <-- close here, not in initialize()
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
}