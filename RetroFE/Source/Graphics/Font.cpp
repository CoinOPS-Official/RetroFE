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
#if (__APPLE__)
    #include <SDL2_ttf/SDL_ttf.h>
#else
    #include <SDL2/SDL_ttf.h>
#endif
#include <cstdio>
#include <cstring>

Font::Font(std::string fontPath, int fontSize, SDL_Color color, int monitor)
    : texture(nullptr)
    , fontPath_(fontPath)
    , fontSize_(fontSize)
    , color_(color)
    , monitor_(monitor)
    {
    }

Font::~Font() {
    deInitialize();
}

SDL_Texture* Font::getTexture() {
    return texture;
}

int Font::getHeight() {
    return height;
}

int Font::getAscent() {
    return ascent;
}

bool Font::getRect(unsigned int charCode, GlyphInfo& glyph) {
    auto it = atlas.find(charCode);
    if (it != atlas.end()) {
        glyph = it->second->glyph;
        return true;
    }
    return false;
}

bool Font::initialize() {
    TTF_Font* font = TTF_OpenFont(fontPath_.c_str(), fontSize_);
    if (!font) {
        LOG_WARNING("Font", "Could not open font: " + std::string(TTF_GetError()));
        return false;
    }

    int x = 0, y = 0, atlasHeight = 0, atlasWidth = 0;
    height = TTF_FontHeight(font);
    ascent = TTF_FontAscent(font);

    for (unsigned short int i = 32; i < 128; ++i) {
        GlyphInfoBuild* info = new GlyphInfoBuild;
        std::memset(info, 0, sizeof(GlyphInfoBuild));

        color_.a = 255;
        info->surface = TTF_RenderGlyph_Blended(font, i, color_);
        TTF_GlyphMetrics(font, i, &info->glyph.minX, &info->glyph.maxX, &info->glyph.minY, &info->glyph.maxY, &info->glyph.advance);

        if (x + info->surface->w >= 1024) {
            atlasHeight += y;
            atlasWidth = std::max(atlasWidth, x);
            x = 0;
            y = 0;
        }

        info->glyph.rect.w = info->surface->w;
        info->glyph.rect.h = info->surface->h;
        info->glyph.rect.x = x;
        info->glyph.rect.y = atlasHeight;
        atlas[i] = info;

        x += info->glyph.rect.w;
        y = std::max(y, info->glyph.rect.h);
    }

    atlasWidth = std::max(atlasWidth, x);
    atlasHeight += y;

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

    SDL_Surface* atlasSurface = SDL_CreateRGBSurface(0, atlasWidth, atlasHeight, 32, rmask, gmask, bmask, amask);
    for (auto& entry : atlas) {
        GlyphInfoBuild* info = entry.second;
        SDL_BlitSurface(info->surface, NULL, atlasSurface, &info->glyph.rect);
        SDL_FreeSurface(info->surface);
        info->surface = NULL;
    }

    SDL_LockMutex(SDL::getMutex());
    texture = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor_), atlasSurface);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(atlasSurface);
    SDL_UnlockMutex(SDL::getMutex());

    TTF_CloseFont(font);
    return true;
}

void Font::deInitialize() {
    if (texture) {
        SDL_LockMutex(SDL::getMutex());
        SDL_DestroyTexture(texture);
        texture = nullptr;
        SDL_UnlockMutex(SDL::getMutex());
    }

    for (auto& entry : atlas) {
        delete entry.second;
    }
    atlas.clear();
}


