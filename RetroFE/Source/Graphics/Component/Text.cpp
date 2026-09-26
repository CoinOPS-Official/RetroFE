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

#include "Text.h"
#include "../Font.h"
#include "../TextEngineAtlas.h"
#include "../../SDL.h"
#include <algorithm>
#include <cmath>

Text::Text(const std::string& text, Page& p, FontManager* font, int monitor)
    : Component(p), textData_(text), fontInst_(font) {
    baseViewInfo.Monitor = monitor;
    baseViewInfo.Layout = page.getCurrentLayout();
}

Text::~Text() { Component::freeGraphicsMemory(); }

void Text::deInitializeFonts() { fontInst_->deInitialize(); }
void Text::initializeFonts() { fontInst_->initialize(); }

void Text::setText(const std::string& text, int id) {
    if (getId() == id && textData_ != text) textData_ = text;
}

const std::string& Text::getText() const { return textData_; }

void Text::draw() {
    Component::draw();
    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font || textData_.empty()) return;

    const float effectiveFontSize = baseViewInfo.FontSize > 0
        ? baseViewInfo.FontSize : static_cast<float>(font->getMaxFontSize());
    const auto* mip = font->getMipLevelForSize(std::ceil(effectiveFontSize));
    if (!mip || mip->fontSize <= 0) return;
    auto* engine = font->getTextEngineAtlas(mip);
    if (!engine) return;

    const float scale = effectiveFontSize / mip->fontSize;
    const float maxW = baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;
    float shapedWidth = 0;
    if (!engine->measure(textData_, scale, shapedWidth)) return;

    const float oldW = baseViewInfo.Width;
    const float oldH = baseViewInfo.Height;
    baseViewInfo.Width = maxW > 0 ? std::min(shapedWidth, maxW) : shapedWidth;
    baseViewInfo.Height = (mip->height + 2 * mip->outlinePx) * scale;
    const float xOrigin = baseViewInfo.XRelativeToOrigin();
    const float yOrigin = baseViewInfo.YRelativeToOrigin();
    baseViewInfo.Width = oldW;
    baseViewInfo.Height = oldH;

    const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
    const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);
    SDL_FRect clip{xOrigin, 0, maxW, static_cast<float>(layoutH)};
    const SDL_FRect* clipPtr = maxW > 0 && shapedWidth > maxW ? &clip : nullptr;
    engine->drawTransformed(textData_, xOrigin, yOrigin, scale,
        baseViewInfo.textColor, baseViewInfo, layoutW, layoutH, clipPtr);
}

bool Text::recycleAsText(const std::string& newText) {
    Component::freeGraphicsMemory();
    textData_ = newText;
    baseViewInfo.ImageWidth = 0;
    baseViewInfo.ImageHeight = 0;
    return true;
}
