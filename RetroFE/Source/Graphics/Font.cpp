/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "Font.h"
#include "TextEngineAtlas.h"
#include "../SDL.h"
#include "../Utility/Log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

FontManager::MipLevel::~MipLevel() {
    delete textEngineAtlas;
    if (outlineFont) TTF_CloseFont(outlineFont);
    if (font) {
        if (fallbackFont) TTF_RemoveFallbackFont(font, fallbackFont);
        TTF_CloseFont(font);
    }
    if (fallbackFont) TTF_CloseFont(fallbackFont);
}

FontManager::FontManager(std::string fontPath, int maxFontSize, SDL_Color color,
    bool gradient, int outlinePx, int monitor, std::string fallbackFontPath)
    : fontPath_(std::move(fontPath))
    , fallbackFontPath_(std::move(fallbackFontPath))
    , maxFontSize_(maxFontSize)
    , color_(color)
    , monitor_(monitor)
    , gradient_(gradient)
    , outlinePx_(std::max(0, outlinePx)) {
}

FontManager::~FontManager() { deInitialize(); }

void FontManager::clearMips() {
    ++resourceGeneration_;
    for (auto& [size, mip] : mipLevels_) delete mip;
    mipLevels_.clear();
    max_height_ = max_ascent_ = 0;
}

bool FontManager::initialize() {
    clearMips();
    if (maxFontSize_ <= 0) return SDL_SetError("Invalid font size");
    preparedSizes_.insert(maxFontSize_);
    for (int size : preparedSizes_) {
        if (!buildMip(size)) {
            clearMips();
            return false;
        }
    }
    return true;
}

void FontManager::deInitialize() { clearMips(); }

bool FontManager::buildMip(int currentSize) {
    if (mipLevels_.count(currentSize)) return true;

    const int outlinePx = outlinePx_ > 0
        ? std::max(1, static_cast<int>(std::lround(double(outlinePx_) * currentSize / maxFontSize_)))
        : 0;
    TTF_Font* font = TTF_OpenFont(fontPath_.c_str(), currentSize);
    if (!font) {
        LOG_WARNING("Font", "Failed to open font '" + fontPath_ + "' at size " +
            std::to_string(currentSize) + ": " + std::string(SDL_GetError()));
        return false;
    }
    TTF_SetFontKerning(font, true);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);

    auto* mip = new MipLevel();
    mip->font = font;
    mip->fontSize = currentSize;
    mip->outlinePx = outlinePx;
    mip->height = TTF_GetFontHeight(font);
    mip->ascent = TTF_GetFontAscent(font);
    mip->descent = TTF_GetFontDescent(font);

    if (outlinePx > 0) {
        mip->outlineFont = TTF_CopyFont(font);
        if (!mip->outlineFont || !TTF_SetFontOutline(mip->outlineFont, outlinePx)) {
            LOG_WARNING("Font", "Failed to prepare outline font at size " +
                std::to_string(currentSize) + ": " + std::string(SDL_GetError()));
            delete mip;
            return false;
        }
    }

    if (!fallbackFontPath_.empty() && std::filesystem::exists(fallbackFontPath_)) {
        mip->fallbackFont = TTF_OpenFont(fallbackFontPath_.c_str(), currentSize);
        if (mip->fallbackFont) {
            TTF_SetFontKerning(mip->fallbackFont, true);
            TTF_SetFontHinting(mip->fallbackFont, TTF_HINTING_LIGHT);
            if (!TTF_AddFallbackFont(font, mip->fallbackFont)) {
                LOG_WARNING("Font", "Failed to add fallback font '" + fallbackFontPath_ +
                    "': " + std::string(SDL_GetError()));
            }
        } else {
            LOG_WARNING("Font", "Failed to open fallback font '" + fallbackFontPath_ +
                "' at size " + std::to_string(currentSize) + ": " + std::string(SDL_GetError()));
        }
    }

    mipLevels_[currentSize] = mip;
    if (currentSize == maxFontSize_) {
        max_height_ = mip->height;
        max_ascent_ = mip->ascent;
    }
    return true;
}

bool FontManager::prepareSize(float size) {
    if (maxFontSize_ <= 0 || !std::isfinite(size) || size <= 0 ||
        double(size) >= std::numeric_limits<int>::max())
        return SDL_SetError("Invalid prepared font size");
    const int rasterSize = static_cast<int>(std::ceil(size));
    if (!buildMip(rasterSize)) return false;
    preparedSizes_.insert(rasterSize);
    return true;
}

bool FontManager::prepareHeight(float height) {
    if (max_height_ <= 0 || !std::isfinite(height) || height <= 0 ||
        double(height) * maxFontSize_ / max_height_ >= std::numeric_limits<int>::max() - 1)
        return false;
    int size = std::max(1, static_cast<int>(std::ceil(double(height) * maxFontSize_ / max_height_)));
    do {
        if (!prepareSize(static_cast<float>(size))) return false;
        if (mipLevels_.at(size)->height >= height) return true;
        ++size;
    } while (size < std::numeric_limits<int>::max());
    return false;
}

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

TextEngineAtlas* FontManager::getTextEngineAtlas(const MipLevel* mip) {
    if (!mip || !mip->font) return nullptr;
    auto* mutableMip = const_cast<MipLevel*>(mip);
    if (!mutableMip->textEngineAtlas) {
        SDL_Renderer* renderer = SDL::getRenderer(monitor_);
        if (!renderer) return nullptr;
        mutableMip->textEngineAtlas = new TextEngineAtlas(renderer, mip->font,
            mip->outlineFont, mip->outlinePx, outlineColor_, gradient_);
    }
    return mutableMip->textEngineAtlas;
}

bool FontManager::prewarmTextEngine(const MipLevel* mip) {
    auto* engine = getTextEngineAtlas(mip);
    return engine && engine->prewarmAscii();
}

void FontManager::setColor(SDL_Color color) { color_ = color; }

int FontManager::getWidth(const std::string& text) {
    const auto it = mipLevels_.find(maxFontSize_);
    return it == mipLevels_.end() ? 0 : getWidth(text, *it->second);
}

int FontManager::getWidth(const std::string& text, const MipLevel& mip) const {
    if (!mip.font) return 0;
    const size_t terminator = text.find('\0');
    const std::string clipped = terminator == std::string::npos
        ? text : text.substr(0, terminator);
    if (auto* engine = const_cast<FontManager*>(this)->getTextEngineAtlas(&mip)) {
        float width = 0;
        if (engine->measure(clipped, 1.0f, width))
            return static_cast<int>(std::lround(width));
    }

    // Measurement may be needed before a renderer exists. Use the same SDL_ttf
    // shaping path without creating a text atlas in that case.
    TTF_Text* shaped = TTF_CreateText(nullptr, mip.font, clipped.c_str(), clipped.size());
    if (!shaped) return 0;
    int width = 0;
    bool hasGlyph = false;
    if (TTF_UpdateText(shaped) && TTF_GetTextSize(shaped, &width, nullptr)) {
        for (int i = 0; i < shaped->internal->num_ops; ++i) {
            if (shaped->internal->ops[i].cmd == TTF_DRAW_COMMAND_COPY) {
                hasGlyph = true;
                break;
            }
        }
        if (hasGlyph) width += 2 * mip.outlinePx;
    }
    TTF_DestroyText(shaped);
    return width;
}

float FontManager::getWidthForHeight(const std::string& text, float height) const {
    const auto* mip = getMipLevelForHeight(height);
    return mip && mip->height > 0 ? getWidth(text, *mip) * height / mip->height : 0.0f;
}

int FontManager::getOutlinePx() const { return outlinePx_; }
