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

#include "ReloadableGlobalHiscores.h"
#include "../ViewInfo.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Database/HiScores.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include "../Font.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <string_view>
#include <limits>

ReloadableGlobalHiscores::ReloadableGlobalHiscores(Configuration& config, std::string textFormat,
    Page& p, int displayOffset, FontManager* font, float scrollingSpeed, float startTime,
    std::string excludedColumns, float baseColumnPadding, float baseRowPadding, size_t maxRows)
    : Component(p)
    , fontInst_(font)
    , textFormat_(textFormat)
    , excludedColumns_(excludedColumns)
    , baseColumnPadding_(baseColumnPadding)
    , baseRowPadding_(baseRowPadding)
    , displayOffset_(displayOffset)
    , maxRows_(maxRows)
    , scrollingSpeed_(scrollingSpeed)
    , currentPosition_(0.0f)
    , startTime_(startTime)
    , waitStartTime_(startTime)
    , waitEndTime_(0.0f)
    , currentTableIndex_(0)
    , tableDisplayTimer_(0.0f)
    , currentTableDisplayTime_(0.0f)
    , displayTime_(5.0f)
    , needsRedraw_(true)
    , lastScale_(0.0f)
    , lastPaddingBetweenColumns_(0.0f)
    , cacheValid_(false)
    , cachedTableIndex_(std::numeric_limits<size_t>::max())
    , cachedTotalTableWidth_(0.0f)
    , visibleColumnIndices_()
    , cachedViewWidth_(-1.0f)
    , cachedBaseFontSize_(-1.0f)
    , lastComputedDrawableHeight_(0.0f)
    , lastComputedRowPadding_(0.0f)
    , lastSelectedItem_(nullptr)
    , intermediateTexture_(nullptr)
    , highScoreTable_(nullptr)
    , headerTexture_(nullptr)
    , tableRowsTexture_(nullptr)
    , tableRowsTextureHeight_(0)
    , headerTextureHeight_(0) {
    // parse excluded list into set
    std::vector<std::string> excludedColumnsVec;
    Utils::listToVector(excludedColumns_, excludedColumnsVec, ',');
    for (auto& colName : excludedColumnsVec) {
        colName = Utils::trimEnds(colName);
        if (!colName.empty())
            excludedColumnsSet_.insert(Utils::toLower(colName));
    }
    allocateGraphicsMemory();
}

ReloadableGlobalHiscores::~ReloadableGlobalHiscores() {
    freeGraphicsMemory();
}

bool ReloadableGlobalHiscores::update(float dt) {
    if (waitEndTime_ > 0.0f) {
        waitEndTime_ -= dt;
        if (waitEndTime_ <= 0.0f) {
            currentPosition_ = 0.0f;
            needsRedraw_ = true;
        }
    }
    else if (waitStartTime_ > 0.0f) {
        waitStartTime_ -= dt;
        needsRedraw_ = true;
    }
    else {
        bool shouldReloadBasedOnParams = false;
        bool resetScrollForParamReload = false;

        if (!cacheValid_) {
            shouldReloadBasedOnParams = true;
            resetScrollForParamReload = true;
        }

        float currentWidthConstraint = baseViewInfo.Width > 0 ? baseViewInfo.Width : baseViewInfo.MaxWidth;
        if (cachedViewWidth_ != currentWidthConstraint && currentWidthConstraint > 0) {
            shouldReloadBasedOnParams = true;
        }
        if (cachedBaseFontSize_ != baseViewInfo.FontSize) {
            shouldReloadBasedOnParams = true;
        }

        if (shouldReloadBasedOnParams && !(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
            if (highScoreTable_ && !highScoreTable_->tables.empty() &&
                cachedTableIndex_ != currentTableIndex_ && cacheValid_) {
                resetScrollForParamReload = true;
            }
            reloadTexture(resetScrollForParamReload);
        }

        if (highScoreTable_ && !highScoreTable_->tables.empty()) {
            if (currentTableIndex_ >= highScoreTable_->tables.size()) {
                currentTableIndex_ = 0;
                if (!(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
                    reloadTexture(true);
                }
            }

            if (cacheValid_ && cachedTableIndex_ == currentTableIndex_) {
                const HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];

                float drawableHeight = lastComputedDrawableHeight_;
                float rowPadding = lastComputedRowPadding_;
                size_t rowsToRender = std::min(table.rows.size(), maxRows_);

                float titleRowHeight = table.id.empty() ? 0.0f : (drawableHeight + rowPadding);
                float columnHeaderRowHeight = (drawableHeight + rowPadding);
                float conceptualHeaderTotalHeight = titleRowHeight + columnHeaderRowHeight;
                float conceptualRowsTotalHeight = (drawableHeight + rowPadding) * static_cast<float>(rowsToRender);
                float totalConceptualTableHeight = conceptualHeaderTotalHeight + conceptualRowsTotalHeight;

                bool needsScrolling = (totalConceptualTableHeight > baseViewInfo.Height);

                if (needsScrolling) {
                    currentPosition_ += scrollingSpeed_ * dt;
                    needsRedraw_ = true;

                    if (currentPosition_ >= totalConceptualTableHeight) {
                        if (highScoreTable_->tables.size() > 1) {
                            currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_->tables.size();
                            waitEndTime_ = startTime_;
                            currentPosition_ = 0.0f;
                            tableDisplayTimer_ = 0.0f;
                            reloadTexture(true);
                        }
                        else {
                            currentPosition_ = 0.0f;
                            waitEndTime_ = startTime_;
                            needsRedraw_ = true;
                        }
                    }
                }
                else {
                    if (currentPosition_ != 0.0f) {
                        currentPosition_ = 0.0f;
                        needsRedraw_ = true;
                    }
                    if (highScoreTable_->tables.size() > 1) {
                        currentTableDisplayTime_ = displayTime_;
                        tableDisplayTimer_ += dt;
                        if (tableDisplayTimer_ >= currentTableDisplayTime_) {
                            currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_->tables.size();
                            tableDisplayTimer_ = 0.0f;
                            waitEndTime_ = startTime_;
                            currentPosition_ = 0.0f;
                            reloadTexture(true);
                        }
                    }
                    else {
                        needsRedraw_ = true;
                    }
                }
            }
            else if (highScoreTable_ && !highScoreTable_->tables.empty()) {
                if (!(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
                    reloadTexture(true);
                }
            }
        }
        else {
            if (cacheValid_) {
                cacheValid_ = false;
                if (headerTexture_ || tableRowsTexture_) {
                    reloadTexture(true);
                }
                needsRedraw_ = true;
            }
        }
    }

    if (newItemSelected || (newScrollItemSelected && getMenuScrollReload())) {
        currentTableIndex_ = 0;
        tableDisplayTimer_ = 0.0f;
        reloadTexture(true);
        newItemSelected = false;
        newScrollItemSelected = false;
    }

    return Component::update(dt);
}

void ReloadableGlobalHiscores::allocateGraphicsMemory() {
    Component::allocateGraphicsMemory();
    reloadTexture();
}

void ReloadableGlobalHiscores::freeGraphicsMemory() {
    Component::freeGraphicsMemory();
    if (headerTexture_) { SDL_DestroyTexture(headerTexture_); headerTexture_ = nullptr; }
    if (tableRowsTexture_) { SDL_DestroyTexture(tableRowsTexture_); tableRowsTexture_ = nullptr; }
    if (intermediateTexture_) { SDL_DestroyTexture(intermediateTexture_); intermediateTexture_ = nullptr; }
}

void ReloadableGlobalHiscores::deInitializeFonts() {
    fontInst_->deInitialize();
}

void ReloadableGlobalHiscores::initializeFonts() {
    fontInst_->initialize();
}

void ReloadableGlobalHiscores::reloadTexture(bool resetScroll) {
    if (resetScroll) {
        currentPosition_ = 0.0f;
        waitStartTime_ = startTime_;
        waitEndTime_ = 0.0f;
    }

    Item* selectedItem = page.getSelectedItem(displayOffset_);
    bool itemChanged = (selectedItem != lastSelectedItem_);

    if (itemChanged) {
        lastSelectedItem_ = selectedItem;
        if (selectedItem) {
            // *** Key difference vs local: ask HiScores for the GLOBAL table built from the Item ***
            highScoreTable_ = HiScores::getInstance().getGlobalHiScoreTable(selectedItem);
            if (highScoreTable_ && !highScoreTable_->tables.empty()) {
                currentTableIndex_ = 0;
            }
        }
        else {
            highScoreTable_ = nullptr;
        }
    }

    if (!highScoreTable_ || highScoreTable_->tables.empty()) {
        if (headerTexture_) { SDL_DestroyTexture(headerTexture_); headerTexture_ = nullptr; }
        if (tableRowsTexture_) { SDL_DestroyTexture(tableRowsTexture_); tableRowsTexture_ = nullptr; }
        cacheValid_ = false;
        needsRedraw_ = true;
        return;
    }

    if (currentTableIndex_ >= highScoreTable_->tables.size()) {
        currentTableIndex_ = 0;
        if (!resetScroll) {
            currentPosition_ = 0.0f;
            waitStartTime_ = startTime_;
            waitEndTime_ = 0.0f;
        }
    }

    const HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];
    if (itemChanged || cachedTableIndex_ != currentTableIndex_) {
        updateVisibleColumns(table);
    }

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    float effectiveViewWidth = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    std::vector<float> colWidths;
    float totalTableWidth = 0;
    float drawableHeight, rowPadding, paddingBetweenColumns;

    float finalScale = computeTableScaleAndWidths(
        font, table,
        drawableHeight, rowPadding, paddingBetweenColumns,
        colWidths, totalTableWidth,
        effectiveViewWidth);

    cachedColumnWidths_ = colWidths;
    cachedTotalTableWidth_ = totalTableWidth;
    lastScale_ = finalScale;
    lastPaddingBetweenColumns_ = paddingBetweenColumns;
    lastComputedDrawableHeight_ = drawableHeight;
    lastComputedRowPadding_ = rowPadding;

    cachedViewWidth_ = effectiveViewWidth;
    cachedBaseFontSize_ = baseViewInfo.FontSize;
    cachedTableIndex_ = currentTableIndex_;
    cacheValid_ = true;

    renderHeaderTexture(font, table, finalScale, drawableHeight, rowPadding, paddingBetweenColumns, totalTableWidth);
    renderTableRowsTexture(font, table, finalScale, drawableHeight, rowPadding, paddingBetweenColumns, totalTableWidth);

    needsRedraw_ = true;
}

void ReloadableGlobalHiscores::draw() {
    Component::draw();
    if (!(highScoreTable_ && !highScoreTable_->tables.empty()) || baseViewInfo.Alpha <= 0.0f) return;
    if (!headerTexture_ || !tableRowsTexture_) return;

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (!renderer) return;

    float compositeWidth = baseViewInfo.Width;
    float compositeHeight = baseViewInfo.Height;

    static int prevW = 0, prevH = 0;
    if (!intermediateTexture_ || prevW != (int)compositeWidth || prevH != (int)compositeHeight) {
        if (intermediateTexture_) SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            (int)compositeWidth, (int)compositeHeight);
        SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
        prevW = (int)compositeWidth;
        prevH = (int)compositeHeight;
    }

    SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, intermediateTexture_);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    float effectiveViewWidth = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;

    float xOrigin = (effectiveViewWidth - cachedTotalTableWidth_) / 2.0f;
    float yOrigin = 0.0f;

    // Header
    SDL_FRect destHeader = { xOrigin, yOrigin, cachedTotalTableWidth_, (float)headerTextureHeight_ };
    SDL_RenderCopyF(renderer, headerTexture_, nullptr, &destHeader);

    // Rows
    float rowsAreaHeight = compositeHeight - headerTextureHeight_;
    float scrollY = currentPosition_;

    if (tableRowsTextureHeight_ <= rowsAreaHeight) {
        SDL_Rect srcRows = { 0, 0, (int)cachedTotalTableWidth_, tableRowsTextureHeight_ };
        SDL_FRect destRows = { xOrigin, yOrigin + headerTextureHeight_, cachedTotalTableWidth_, (float)tableRowsTextureHeight_ };
        SDL_RenderCopyF(renderer, tableRowsTexture_, &srcRows, &destRows);
    }
    else {
        if (scrollY < tableRowsTextureHeight_) {
            int visibleSrcHeight = (int)std::min(rowsAreaHeight, (float)(tableRowsTextureHeight_ - (int)scrollY));
            if (visibleSrcHeight > 0) {
                SDL_Rect srcRows = { 0, (int)scrollY, (int)cachedTotalTableWidth_, visibleSrcHeight };
                SDL_FRect destRows = { xOrigin, yOrigin + headerTextureHeight_, cachedTotalTableWidth_, (float)visibleSrcHeight };
                SDL_RenderCopyF(renderer, tableRowsTexture_, &srcRows, &destRows);
            }
        }
    }

#ifndef NDEBUG
    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
    SDL_Rect outlineRect = { 0, 0, (int)compositeWidth - 1, (int)compositeHeight - 1 };
    SDL_RenderDrawRect(renderer, &outlineRect);
#endif

    SDL_SetRenderTarget(renderer, oldTarget);

    SDL_FRect rect = {
        baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
        baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight()
    };
    SDL::renderCopyF(intermediateTexture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
}

float ReloadableGlobalHiscores::computeTableScaleAndWidths(
    FontManager* font,
    const HighScoreTable& table,
    float& outDrawableHeight,
    float& outRowPadding,
    float& outPaddingBetweenColumns,
    std::vector<float>& outColumnWidths,
    float& outTotalTableWidth,
    float widthConstraint) {
    float initialScale = baseViewInfo.FontSize / (float)font->getHeight();
    float drawableHeight = font->getAscent() * initialScale;
    float rowPadding = baseRowPadding_ * drawableHeight;
    float paddingBetweenColumns = baseColumnPadding_ * drawableHeight;

    outColumnWidths.clear();
    outTotalTableWidth = 0.0f;

    for (size_t visibleIndex = 0; visibleIndex < visibleColumnIndices_.size(); ++visibleIndex) {
        size_t colIndex = visibleColumnIndices_[visibleIndex];
        float maxColumnWidth = 0.0f;

        if (colIndex < table.columns.size())
            maxColumnWidth = std::max(maxColumnWidth, (float)font->getWidth(table.columns[colIndex]) * initialScale);

        for (const auto& row : table.rows) {
            if (colIndex < row.size())
                maxColumnWidth = std::max(maxColumnWidth, (float)font->getWidth(row[colIndex]) * initialScale);
        }

        outColumnWidths.push_back(maxColumnWidth);
        outTotalTableWidth += maxColumnWidth + paddingBetweenColumns;
    }
    if (!outColumnWidths.empty())
        outTotalTableWidth -= paddingBetweenColumns;

    float scale = initialScale;
    if (outTotalTableWidth > widthConstraint) {
        float downScaleFactor = widthConstraint / outTotalTableWidth;
        scale = initialScale * downScaleFactor;

        drawableHeight = font->getAscent() * scale;
        rowPadding = baseRowPadding_ * drawableHeight;
        paddingBetweenColumns = baseColumnPadding_ * drawableHeight;

        outColumnWidths.clear();
        outTotalTableWidth = 0.0f;
        for (size_t visibleIndex = 0; visibleIndex < visibleColumnIndices_.size(); ++visibleIndex) {
            size_t colIndex = visibleColumnIndices_[visibleIndex];
            float maxColumnWidth = 0.0f;

            if (colIndex < table.columns.size())
                maxColumnWidth = std::max(maxColumnWidth, (float)font->getWidth(table.columns[colIndex]) * scale);

            for (const auto& row : table.rows) {
                if (colIndex < row.size())
                    maxColumnWidth = std::max(maxColumnWidth, (float)font->getWidth(row[colIndex]) * scale);
            }

            outColumnWidths.push_back(maxColumnWidth);
            outTotalTableWidth += maxColumnWidth + paddingBetweenColumns;
        }
        if (!outColumnWidths.empty())
            outTotalTableWidth -= paddingBetweenColumns;
    }

    outDrawableHeight = drawableHeight;
    outRowPadding = rowPadding;
    outPaddingBetweenColumns = paddingBetweenColumns;
    return scale;
}

void ReloadableGlobalHiscores::updateVisibleColumns(const HighScoreTable& table) {
    visibleColumnIndices_.clear();

    for (size_t colIndex = 0; colIndex < table.columns.size(); ++colIndex) {
        const std::string& columnName = table.columns[colIndex];
        std::string columnNameLower = Utils::toLower(columnName);

        bool isExcluded = std::any_of(
            excludedColumnsSet_.begin(),
            excludedColumnsSet_.end(),
            [&](std::string_view prefix) {
                return columnNameLower.compare(0, prefix.size(), prefix) == 0;
            });

        if (!isExcluded) visibleColumnIndices_.push_back(colIndex);
    }
}

void ReloadableGlobalHiscores::renderHeaderTexture(
    FontManager* font, const HighScoreTable& table, float scale, float drawableHeight,
    float rowPadding, float paddingBetweenColumns, float totalTableWidth) {
    if (headerTexture_) SDL_DestroyTexture(headerTexture_);
    headerTexture_ = nullptr;

    int headerTexHeight = 0;
    if (!table.id.empty()) headerTexHeight += (int)(drawableHeight + rowPadding);
    headerTexHeight += (int)(drawableHeight + rowPadding);
    headerTextureHeight_ = headerTexHeight;

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    headerTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        (int)totalTableWidth, headerTextureHeight_);
    SDL_SetTextureBlendMode(headerTexture_, SDL_BLENDMODE_BLEND);

    SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, headerTexture_);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    float xOrigin = 0.0f;
    float y = 0.0f;

    if (!table.id.empty()) {
        float titleWidth = (float)font->getWidth(table.id) * scale;
        float titleX = (totalTableWidth - titleWidth) / 2.0f;
        for (char c : table.id) {
            FontManager::GlyphInfo glyph;
            if (font->getRect(c, glyph)) {
                SDL_Rect src = glyph.rect;
                SDL_FRect dst = { titleX, y, glyph.rect.w * scale, glyph.rect.h * scale };
                SDL_RenderCopyF(renderer, font->getTexture(), &src, &dst);
                titleX += (float)glyph.advance * scale;
            }
        }
        y += drawableHeight + rowPadding;
    }

    float x = xOrigin;
    for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
        size_t colIndex = visibleColumnIndices_[i];
        const std::string& header = table.columns[colIndex];
        float headerWidth = (float)font->getWidth(header) * scale;
        float xAligned = x + (cachedColumnWidths_[i] - headerWidth) / 2.0f;
        float charX = xAligned;
        for (char c : header) {
            FontManager::GlyphInfo glyph;
            if (font->getRect(c, glyph)) {
                SDL_Rect src = glyph.rect;
                SDL_FRect dst = { charX, y, glyph.rect.w * scale, glyph.rect.h * scale };
                SDL_RenderCopyF(renderer, font->getTexture(), &src, &dst);
                charX += (float)glyph.advance * scale;
            }
        }
        x += cachedColumnWidths_[i] + paddingBetweenColumns;
    }

#ifndef NDEBUG
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_Rect outlineRect = { 0, 0, (int)totalTableWidth, headerTextureHeight_ };
    SDL_RenderDrawRect(renderer, &outlineRect);
#endif
    SDL_SetRenderTarget(renderer, oldTarget);
}

void ReloadableGlobalHiscores::renderTableRowsTexture(
    FontManager* font, const HighScoreTable& table, float scale, float drawableHeight,
    float rowPadding, float paddingBetweenColumns, float totalTableWidth) {
    if (tableRowsTexture_) SDL_DestroyTexture(tableRowsTexture_);
    tableRowsTexture_ = nullptr;

    size_t numRows = table.rows.size();
    size_t rowsToActuallyRender = std::min(numRows, maxRows_);

    tableRowsTextureHeight_ = (int)((drawableHeight + rowPadding) * rowsToActuallyRender);
    if (tableRowsTextureHeight_ <= 0) tableRowsTextureHeight_ = 1;

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    tableRowsTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        (int)totalTableWidth, tableRowsTextureHeight_);
    SDL_SetTextureBlendMode(tableRowsTexture_, SDL_BLENDMODE_BLEND);

    SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, tableRowsTexture_);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    float xOrigin = 0.0f;
    for (int rowIndex = 0; rowIndex < (int)rowsToActuallyRender; ++rowIndex) {
        float y = (drawableHeight + rowPadding) * rowIndex;
        float x = xOrigin;
        for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
            size_t colIndex = visibleColumnIndices_[i];
            if (colIndex >= table.rows[rowIndex].size()) continue;
            const std::string& cell = table.rows[rowIndex][colIndex];
            float cellWidth = (float)font->getWidth(cell) * scale;
            float xAligned = x + (cachedColumnWidths_[i] - cellWidth) / 2.0f;
            float charX = xAligned;
            for (char c : cell) {
                FontManager::GlyphInfo glyph;
                if (font->getRect(c, glyph)) {
                    SDL_Rect src = glyph.rect;
                    SDL_FRect dst = { charX, y, glyph.rect.w * scale, glyph.rect.h * scale };
                    SDL_RenderCopyF(renderer, font->getTexture(), &src, &dst);
                    charX += (float)glyph.advance * scale;
                }
            }
            x += cachedColumnWidths_[i] + paddingBetweenColumns;
        }
    }

#ifndef NDEBUG
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_Rect outlineRect = { 0, 0, (int)totalTableWidth, tableRowsTextureHeight_ };
    SDL_RenderDrawRect(renderer, &outlineRect);
#endif
    SDL_SetRenderTarget(renderer, oldTarget);
}
