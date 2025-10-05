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

#include "ReloadableHiscores.h"
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

ReloadableHiscores::ReloadableHiscores(Configuration& config, std::string textFormat,
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
	// Parse the excluded columns
	std::vector<std::string> excludedColumnsVec;
	Utils::listToVector(excludedColumns_, excludedColumnsVec, ',');

	// Trim whitespace, convert to lowercase, and populate the unordered_set
	for (auto& colName : excludedColumnsVec) {
		colName = Utils::trimEnds(colName);
		if (!colName.empty()) {
			excludedColumnsSet_.insert(Utils::toLower(colName));
		}
	}

	allocateGraphicsMemory();
}



ReloadableHiscores::~ReloadableHiscores(){
	freeGraphicsMemory();
	//deInitializeFonts();
}



bool ReloadableHiscores::update(float dt) {
	if (waitEndTime_ > 0.0f) {
		waitEndTime_ -= dt;
		if (waitEndTime_ <= 0.0f) {
			// Ready to start scrolling again or display next static table
			currentPosition_ = 0.0f; // Start from the top
			needsRedraw_ = true;
			LOG_DEBUG("ReloadableHiscores", "Wait time ended.");
		}
		// While waiting, no other updates to scrolling or table logic
	}
	else if (waitStartTime_ > 0.0f) {
		waitStartTime_ -= dt;
		// Still in initial wait, no scrolling yet
		needsRedraw_ = true; // Keep drawing current state during initial wait
	}
	else {
		// --- Main Update Logic (Not in initial wait or end-of-scroll wait) ---

		// --- 1. Check for conditions requiring texture/layout recalculation ---
		bool shouldReloadBasedOnParams = false;
		bool resetScrollForParamReload = false;

		if (!cacheValid_) { // Cache is not valid (e.g., initial load, or explicitly invalidated)
			shouldReloadBasedOnParams = true;
			resetScrollForParamReload = true; // Full reload, reset scroll
			LOG_DEBUG("ReloadableHiscores", "Cache invalid, scheduling reload.");
		}

		// Check for view changes
		float currentWidthConstraint = baseViewInfo.Width > 0 ? baseViewInfo.Width : baseViewInfo.MaxWidth;
		if (cachedViewWidth_ != currentWidthConstraint && currentWidthConstraint > 0) {
			shouldReloadBasedOnParams = true;
			// resetScrollForParamReload = false; // Viewport change shouldn't reset scroll if content is same
			LOG_DEBUG("ReloadableHiscores", "View width changed, scheduling reload.");
		}
		if (cachedBaseFontSize_ != baseViewInfo.FontSize) {
			shouldReloadBasedOnParams = true;
			// resetScrollForParamReload = false; // Font size change shouldn't reset scroll
			LOG_DEBUG("ReloadableHiscores", "Base font size changed, scheduling reload.");
		}
		// Potentially add FontManager* check if baseViewInfo.font can change instance

		// If an item is newly selected, it will trigger its own reload at the end of update().
		// So, only proceed with param-based reload if no new item selection is pending.
		if (shouldReloadBasedOnParams && !(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
			// If currentTableIndex_ also changed (e.g. externally), ensure scroll resets
			if (highScoreTable_ && !highScoreTable_->tables.empty() &&
				cachedTableIndex_ != currentTableIndex_ && cacheValid_) {
				resetScrollForParamReload = true;
			}
			reloadTexture(resetScrollForParamReload);
			// reloadTexture sets cacheValid_ = true if successful for a table
		}

		// --- 2. Scrolling and Table Switching Logic ---
		if (highScoreTable_ && !highScoreTable_->tables.empty()) {
			// Ensure currentTableIndex_ is within bounds before use
			if (currentTableIndex_ >= highScoreTable_->tables.size()) {
				LOG_WARNING("ReloadableHiscores", "currentTableIndex_ was out of bounds, resetting to 0.");
				currentTableIndex_ = 0;
				// This change in currentTableIndex_ should have triggered a reload if cache was valid for old index.
				// Or will trigger one in the next frame if `shouldReloadBasedOnParams` didn't catch it.
				// For safety, if it was out of bounds and cache might be stale:
				if (!shouldReloadBasedOnParams && !(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
					reloadTexture(true); // Force reload for the new valid index
				}
			}

			// Proceed only if cache is valid for the current table
			// (reloadTexture above, or from a previous frame, should have made it valid)
			if (cacheValid_ && cachedTableIndex_ == currentTableIndex_) {
				const HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];

				// Use authoritative geometric values from cache
				float drawableHeight = lastComputedDrawableHeight_;
				float rowPadding = lastComputedRowPadding_;
				// paddingBetweenColumns is used by draw, not directly here for height

				size_t rowsToRender = std::min(table.rows.size(), maxRows_);

				// Calculate conceptual heights based on cached scaled values
				float titleRowHeight = table.id.empty() ? 0.0f : (drawableHeight + rowPadding);
				float columnHeaderRowHeight = (drawableHeight + rowPadding); // Assumes headers always take one row height

				// This is the total height of the static header part (title + column names)
				// It should match headerTextureHeight_ if rendering was successful.
				float conceptualHeaderTotalHeight = titleRowHeight + columnHeaderRowHeight;

				// Height of all renderable rows
				float conceptualRowsTotalHeight = (drawableHeight + rowPadding) * static_cast<float>(rowsToRender);

				// Total conceptual height of the table content to be displayed
				float totalConceptualTableHeight = conceptualHeaderTotalHeight + conceptualRowsTotalHeight;

				// The height used for scroll completion in the original code seemed to be this total conceptual height.
				float scrollCompletionTarget = totalConceptualTableHeight;

				// LOG_DEBUG("ReloadableHiscores", "Total Conceptual Table Height: " + std::to_string(totalConceptualTableHeight));
				// LOG_DEBUG("ReloadableHiscores", "Scroll Completion Target: " + std::to_string(scrollCompletionTarget));

				// Determine if scrolling is required based on actual rendered texture heights vs view.
				// More accurately, compare conceptual total height to available view height.
				bool needsScrolling = (totalConceptualTableHeight > baseViewInfo.Height);

				if (needsScrolling) {
					currentPosition_ += scrollingSpeed_ * dt;
					needsRedraw_ = true; // Keep redrawing while scrolling

					// LOG_DEBUG("ReloadableHiscores", "Scrolling... Current Position: " + std::to_string(currentPosition_));

					if (currentPosition_ >= scrollCompletionTarget) {
						if (highScoreTable_->tables.size() > 1) {
							currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_->tables.size();
							waitEndTime_ = startTime_;    // Pause before next table starts scrolling
							currentPosition_ = 0.0f;      // Reset scroll for the new table
							tableDisplayTimer_ = 0.0f;
							reloadTexture(true);          // Reload textures for the new table
							LOG_INFO("ReloadableHiscores", "Switched to table index (scrolling): " + std::to_string(currentTableIndex_));
						}
						else { // Single table, reset scroll
							currentPosition_ = 0.0f;
							waitEndTime_ = startTime_;    // Pause before scrolling starts again
							needsRedraw_ = true;
							LOG_INFO("ReloadableHiscores", "Scroll reset for single scrolling table.");
						}
					}
				}
				else { // Not scrolling (table fits or only one page)
					// Ensure it's at the top if it's not supposed to scroll
					if (currentPosition_ != 0.0f) {
						currentPosition_ = 0.0f;
						needsRedraw_ = true;
					}

					// Handle multi-table switching for static (non-scrolling) tables
					if (highScoreTable_->tables.size() > 1) {
						currentTableDisplayTime_ = displayTime_; // Static display time
						tableDisplayTimer_ += dt;

						// LOG_DEBUG("ReloadableHiscores", "Static Table Display Timer: " + std::to_string(tableDisplayTimer_) + " / " + std::to_string(currentTableDisplayTime_));

						if (tableDisplayTimer_ >= currentTableDisplayTime_) {
							currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_->tables.size();
							tableDisplayTimer_ = 0.0f;
							waitEndTime_ = startTime_; // Optional: Add a pause before showing next static table
							currentPosition_ = 0.0f;   // Ensure it's at top
							reloadTexture(true);       // Reload for the new static table
							LOG_INFO("ReloadableHiscores", "Switched to table index (static): " + std::to_string(currentTableIndex_));
						}
					}
					else {
						// Single static table, do nothing beyond ensuring it's drawn
						needsRedraw_ = true;
					}
				}
			}
			else if (highScoreTable_ && !highScoreTable_->tables.empty()) {
				// highScoreTable exists, but cache is not valid for currentTableIndex_ OR cacheValid is false.
				// This implies a reload is needed if not already handled by newItemSelected.
				if (!(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
					LOG_DEBUG("ReloadableHiscores", "Cache invalid or mismatched for current table index. Forcing reload.");
					reloadTexture(true); // Force reload for the current table index
				}
			}
		}
		else { // No highScoreTable_ or it's empty
			if (cacheValid_) { // If cache was previously valid, invalidate it now
				LOG_DEBUG("ReloadableHiscores", "No high score table, invalidating cache.");
				cacheValid_ = false;
				// Clear textures if they exist, handled by reloadTexture when highScoreTable_ is null
				if (headerTexture_ || tableRowsTexture_) {
					reloadTexture(true); // This will clear textures because highScoreTable_ is null/empty
				}
				needsRedraw_ = true; // Redraw to clear any old table
			}
		}
	} // End of main update logic (after waits)

	// --- 3. Handle New Item Selection (takes precedence and re-initializes) ---
	if (newItemSelected || (newScrollItemSelected && getMenuScrollReload())) {
		LOG_INFO("ReloadableHiscores", "New item selected. Resetting and reloading.");
		currentTableIndex_ = 0;      // Always start with the first table for a new item
		tableDisplayTimer_ = 0.0f;   // Reset display timer
		// currentPosition_ and wait timers will be reset by reloadTexture(true)
		reloadTexture(true);         // This reloads data, recalculates layout, resets scroll/wait

		newItemSelected = false;
		newScrollItemSelected = false;
		// needsRedraw_ is set by reloadTexture
	}

	return Component::update(dt);
}

void ReloadableHiscores::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	reloadTexture();
}


void ReloadableHiscores::freeGraphicsMemory() {
	Component::freeGraphicsMemory();
	if (headerTexture_) { SDL_DestroyTexture(headerTexture_); headerTexture_ = nullptr; }
	if (tableRowsTexture_) { SDL_DestroyTexture(tableRowsTexture_); tableRowsTexture_ = nullptr; }
	if (intermediateTexture_) { SDL_DestroyTexture(intermediateTexture_); intermediateTexture_ = nullptr; }
}


void ReloadableHiscores::deInitializeFonts() {
	fontInst_->deInitialize();
}


void ReloadableHiscores::initializeFonts() {
	fontInst_->initialize();
}


void ReloadableHiscores::reloadTexture(bool resetScroll) {
	
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
			highScoreTable_ = HiScores::getInstance().getHighScoreTable(selectedItem->name);
			if (highScoreTable_ && !highScoreTable_->tables.empty()) {
				currentTableIndex_ = 0; // Reset to first table
			}
		}
		else {
			highScoreTable_ = nullptr;
		}
		// Invalidate cache fully if item changes, forcing re-evaluation of everything
		// for the new table or lack thereof.
		// cachedTableIndex_ is effectively invalidated by currentTableIndex_ possibly changing.
		// And we'll update it below after calculations.
	}

	// If no table, clear textures and bail
	if (!highScoreTable_ || highScoreTable_->tables.empty()) {
		if (headerTexture_) { SDL_DestroyTexture(headerTexture_); headerTexture_ = nullptr; }
		if (tableRowsTexture_) { SDL_DestroyTexture(tableRowsTexture_); tableRowsTexture_ = nullptr; }
		cacheValid_ = false; // No valid table data to cache
		needsRedraw_ = true;
		return;
	}

	// Ensure currentTableIndex_ is valid *before* using it.
	if (currentTableIndex_ >= highScoreTable_->tables.size()) {
		currentTableIndex_ = 0;
		// This implies a table switch, scroll should ideally reset if not already handled
		if (!resetScroll) { // If resetScroll wasn't already true from args
			currentPosition_ = 0.0f;
			waitStartTime_ = startTime_;
			waitEndTime_ = 0.0f;
		}
	}

	const HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];
	if (itemChanged || cachedTableIndex_ != currentTableIndex_) { // Update visible columns if item or table index changed
		updateVisibleColumns(table);
	}


	FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
	float effectiveViewWidth = baseViewInfo.MaxWidth; // Default to MaxWidth
	if (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth) {
		effectiveViewWidth = baseViewInfo.Width;
	}
	std::vector<float> colWidths;
	float totalTableWidth = 0;
	float drawableHeight, rowPadding, paddingBetweenColumns; // These are 'out' params

	float finalScale = computeTableScaleAndWidths(
		font, table,
		drawableHeight, rowPadding, paddingBetweenColumns, // Pass by ref to be filled
		colWidths, totalTableWidth,
		effectiveViewWidth);

	// Store all authoritative calculated values
	cachedColumnWidths_ = colWidths;
	cachedTotalTableWidth_ = totalTableWidth;
	lastScale_ = finalScale;
	lastPaddingBetweenColumns_ = paddingBetweenColumns;
	lastComputedDrawableHeight_ = drawableHeight;
	lastComputedRowPadding_ = rowPadding;

	// Cache the context under which these values were computed
	cachedViewWidth_ = effectiveViewWidth;
	cachedBaseFontSize_ = baseViewInfo.FontSize;
	cachedTableIndex_ = currentTableIndex_; // The table for which this cache is valid
	cacheValid_ = true;                     // Mark cache as valid

	// Render textures using these authoritative values
	renderHeaderTexture(font, table, finalScale, drawableHeight, rowPadding, paddingBetweenColumns, totalTableWidth);
	renderTableRowsTexture(font, table, finalScale, drawableHeight, rowPadding, paddingBetweenColumns, totalTableWidth);

	needsRedraw_ = true;
}

void ReloadableHiscores::draw() {
	Component::draw();

	if (!(highScoreTable_ && !highScoreTable_->tables.empty()) || baseViewInfo.Alpha <= 0.0f)
		return;
	if (!headerTexture_ || !tableRowsTexture_) return;

	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	if (!renderer) return;

	// Compute composite/intermediate texture size (covering the whole visible region)
	float compositeWidth = baseViewInfo.Width;
	float compositeHeight = baseViewInfo.Height;

	// (Re)create intermediate texture if needed
	static int prevW = 0, prevH = 0;
	if (!intermediateTexture_ || prevW != (int)compositeWidth || prevH != (int)compositeHeight) {
		if (intermediateTexture_) SDL_DestroyTexture(intermediateTexture_);
		intermediateTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
			(int)compositeWidth, (int)compositeHeight);
		SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
		prevW = (int)compositeWidth;
		prevH = (int)compositeHeight;
	}

	// Draw header/body into intermediate texture
	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, intermediateTexture_);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);

	float effectiveViewWidth = baseViewInfo.MaxWidth; // Default to MaxWidth
	if (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
		effectiveViewWidth = baseViewInfo.Width;

	float xOrigin = (effectiveViewWidth - cachedTotalTableWidth_) / 2.0f;
	float yOrigin = 0.0f; // Always draw header at the top of intermediate

	// -- Draw header --
	SDL_FRect destHeader = {
		xOrigin,
		yOrigin,
		cachedTotalTableWidth_,
		static_cast<float>(headerTextureHeight_)
	};
	SDL_RenderCopyF(renderer, headerTexture_, nullptr, &destHeader);

	// -- Draw table body --
	float rowsAreaHeight = compositeHeight - headerTextureHeight_;
	float scrollY = currentPosition_;

	if (tableRowsTextureHeight_ <= rowsAreaHeight) {
		// NON-SCROLLING
		SDL_Rect srcRows = { 0, 0, static_cast<int>(cachedTotalTableWidth_), tableRowsTextureHeight_ };
		SDL_FRect destRows = { xOrigin, yOrigin + headerTextureHeight_, cachedTotalTableWidth_, static_cast<float>(tableRowsTextureHeight_) };
		SDL_RenderCopyF(renderer, tableRowsTexture_, &srcRows, &destRows);
	}
	else {
		// SCROLLING
		if (scrollY < tableRowsTextureHeight_) {
			int visibleSrcHeight = static_cast<int>(std::min(rowsAreaHeight, tableRowsTextureHeight_ - scrollY));
			if (visibleSrcHeight > 0) {
				SDL_Rect srcRows = {
					0,
					static_cast<int>(scrollY),
					static_cast<int>(cachedTotalTableWidth_),
					visibleSrcHeight
				};
				SDL_FRect destRows = {
					xOrigin,
					yOrigin + headerTextureHeight_,
					cachedTotalTableWidth_,
					static_cast<float>(visibleSrcHeight)
				};
				SDL_RenderCopyF(renderer, tableRowsTexture_, &srcRows, &destRows);
			}
		}
	}

#ifndef NDEBUG
	SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Green, opaque
	SDL_Rect outlineRect = { 0, 0, static_cast<int>(compositeWidth) - 1, static_cast<int>(compositeHeight) - 1 };
	SDL_RenderDrawRect(renderer, &outlineRect);
#endif
	SDL_SetRenderTarget(renderer, oldTarget);

	// Final: Draw the intermediate texture to the real target using SDL::renderCopyF (handles alpha/rotation/etc)
	SDL_FRect rect = {
		baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
		baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };

	SDL::renderCopyF(intermediateTexture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
		page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
		page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
}


// Returns final scale and updates column widths and total width
// Returns final scale and updates column widths and total width
float ReloadableHiscores::computeTableScaleAndWidths(
	FontManager* font,
	const HighScoreTable& table,
	float& outDrawableHeight,
	float& outRowPadding,
	float& outPaddingBetweenColumns,
	std::vector<float>& outColumnWidths,
	float& outTotalTableWidth,
	float widthConstraint) {

	// MODIFIED: Use max-resolution metrics for a stable, high-precision baseline.
	float initialScale = baseViewInfo.FontSize / static_cast<float>(font->getMaxHeight());
	float drawableHeight = font->getMaxAscent() * initialScale;
	float rowPadding = baseRowPadding_ * drawableHeight;
	float paddingBetweenColumns = baseColumnPadding_ * drawableHeight;

	// Measure column widths using the high-res font data (font->getWidth now does this automatically)
	outColumnWidths.clear();
	outTotalTableWidth = 0.0f;
	for (size_t visibleIndex = 0; visibleIndex < visibleColumnIndices_.size(); ++visibleIndex) {
		size_t colIndex = visibleColumnIndices_[visibleIndex];
		float maxColumnWidth = 0.0f;
		// Header
		if (colIndex < table.columns.size())
			maxColumnWidth = std::max(maxColumnWidth, float(font->getWidth(table.columns[colIndex])) * initialScale);
		// Rows
		for (const auto& row : table.rows) {
			if (colIndex < row.size())
				maxColumnWidth = std::max(maxColumnWidth, float(font->getWidth(row[colIndex])) * initialScale);
		}
		outColumnWidths.push_back(maxColumnWidth);
		outTotalTableWidth += maxColumnWidth + paddingBetweenColumns;
	}
	if (!outColumnWidths.empty())
		outTotalTableWidth -= paddingBetweenColumns; // Remove last extra padding

	// If the table is too wide, calculate a downscale factor
	float scale = initialScale;
	if (outTotalTableWidth > widthConstraint && outTotalTableWidth > 0) {
		float downScaleFactor = widthConstraint / outTotalTableWidth;
		scale = initialScale * downScaleFactor;

		// Re-calculate all metrics using the final, downscaled value
		drawableHeight = font->getMaxAscent() * scale; // MODIFIED: Use getMaxAscent
		rowPadding = baseRowPadding_ * drawableHeight;
		paddingBetweenColumns = baseColumnPadding_ * drawableHeight;
		outColumnWidths.clear();
		outTotalTableWidth = 0.0f;
		for (size_t visibleIndex = 0; visibleIndex < visibleColumnIndices_.size(); ++visibleIndex) {
			size_t colIndex = visibleColumnIndices_[visibleIndex];
			float maxColumnWidth = 0.0f;
			if (colIndex < table.columns.size())
				maxColumnWidth = std::max(maxColumnWidth, float(font->getWidth(table.columns[colIndex])) * scale);
			for (const auto& row : table.rows) {
				if (colIndex < row.size())
					maxColumnWidth = std::max(maxColumnWidth, float(font->getWidth(row[colIndex])) * scale);
			}
			outColumnWidths.push_back(maxColumnWidth);
			outTotalTableWidth += maxColumnWidth + paddingBetweenColumns;
		}
		if (!outColumnWidths.empty())
			outTotalTableWidth -= paddingBetweenColumns;
	}

	// Output the final calculated values
	outDrawableHeight = drawableHeight;
	outRowPadding = rowPadding;
	outPaddingBetweenColumns = paddingBetweenColumns;
	return scale;
}


void ReloadableHiscores::updateVisibleColumns(const HighScoreTable& table) {
	visibleColumnIndices_.clear();

	for (size_t colIndex = 0; colIndex < table.columns.size(); ++colIndex) {
		const std::string& columnName = table.columns[colIndex];
		std::string columnNameLower = Utils::toLower(columnName);

		// Check if any excluded prefix is a prefix of the column name
		bool isExcluded = std::any_of(
			excludedColumnsSet_.begin(),
			excludedColumnsSet_.end(),
			[&](std::string_view prefix) {
				return columnNameLower.compare(0, prefix.size(), prefix) == 0;
			}
		);

		if (!isExcluded) {
			visibleColumnIndices_.push_back(colIndex);
		}
	}
}

void ReloadableHiscores::renderHeaderTexture(
	FontManager* font, const HighScoreTable& table, float scale, float drawableHeight, float rowPadding, float paddingBetweenColumns, float totalTableWidth) {
	if (headerTexture_) SDL_DestroyTexture(headerTexture_);
	headerTexture_ = nullptr;

	int headerTexHeight = 0;
	if (!table.id.empty()) headerTexHeight += static_cast<int>(drawableHeight + rowPadding);
	headerTexHeight += static_cast<int>(drawableHeight + rowPadding);
	if (headerTexHeight <= 0) headerTexHeight = 1; // Safety for 0-height texture
	headerTextureHeight_ = headerTexHeight;

	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	headerTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
		static_cast<int>(totalTableWidth), headerTextureHeight_);
	SDL_SetTextureBlendMode(headerTexture_, SDL_BLENDMODE_BLEND);

	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, headerTexture_);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);

	// Mipmapping Setup
	const float targetPixelHeight = scale * font->getMaxHeight();
	const FontManager::MipLevel* mip = font->getMipLevelForSize(static_cast<int>(targetPixelHeight));
	if (!mip) {
		SDL_SetRenderTarget(renderer, oldTarget);
		return;
	}
	const float mipRelativeScale = (mip->height > 0) ? (targetPixelHeight / mip->height) : 1.0f;
	SDL_Texture* tex = mip->fillTexture;

	float y = 0.0f; // This is the baseline y for the current row.

	// Title
	if (!table.id.empty()) {
		float titleWidth = static_cast<float>(font->getWidth(table.id)) * scale;
		float titleX = (totalTableWidth - titleWidth) / 2.0f;
		float penX = titleX;
		for (char c : table.id) {
			auto it = mip->glyphs.find(c);
			if (it != mip->glyphs.end()) {
				const auto& g = it->second;
				SDL_Rect src = g.rect;
				// CORRECTED: Use the row's 'y' directly. Do not recalculate per-glyph vertical alignment.
				SDL_FRect dst = { penX, y, g.rect.w * mipRelativeScale, g.rect.h * mipRelativeScale };
				SDL_RenderCopyF(renderer, tex, &src, &dst);
				penX += g.advance * mipRelativeScale;
			}
		}
		y += drawableHeight + rowPadding;
	}

	// Headers
	float x = 0.0f;
	for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
		size_t colIndex = visibleColumnIndices_[i];
		const std::string& header = table.columns[colIndex];
		float headerWidth = static_cast<float>(font->getWidth(header)) * scale;
		float xAligned = x + (cachedColumnWidths_[i] - headerWidth) / 2.0f;
		float penX = xAligned;
		for (char c : header) {
			auto it = mip->glyphs.find(c);
			if (it != mip->glyphs.end()) {
				const auto& g = it->second;
				SDL_Rect src = g.rect;
				// CORRECTED: Use the row's 'y' directly.
				SDL_FRect dst = { penX, y, g.rect.w * mipRelativeScale, g.rect.h * mipRelativeScale };
				SDL_RenderCopyF(renderer, tex, &src, &dst);
				penX += g.advance * mipRelativeScale;
			}
		}
		x += cachedColumnWidths_[i] + paddingBetweenColumns;
	}

#ifndef NDEBUG
	SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red, opaque
	SDL_Rect outlineRect = { 0, 0, static_cast<int>(totalTableWidth) - 1, headerTextureHeight_ - 1 };
	SDL_RenderDrawRect(renderer, &outlineRect);
#endif
	SDL_SetRenderTarget(renderer, oldTarget);
}

void ReloadableHiscores::renderTableRowsTexture(
	FontManager* font, const HighScoreTable& table, float scale, float drawableHeight, float rowPadding, float paddingBetweenColumns, float totalTableWidth) {
	if (tableRowsTexture_) SDL_DestroyTexture(tableRowsTexture_);
	tableRowsTexture_ = nullptr;

	size_t numRows = table.rows.size();
	size_t rowsToActuallyRender = std::min(numRows, maxRows_);

	tableRowsTextureHeight_ = static_cast<int>((drawableHeight + rowPadding) * rowsToActuallyRender);
	if (tableRowsTextureHeight_ <= 0) tableRowsTextureHeight_ = 1; // Avoid 0-height texture

	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	tableRowsTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
		static_cast<int>(totalTableWidth), tableRowsTextureHeight_);
	SDL_SetTextureBlendMode(tableRowsTexture_, SDL_BLENDMODE_BLEND);

	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, tableRowsTexture_);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);

	// Mipmapping Setup
	const float targetPixelHeight = scale * font->getMaxHeight();
	const FontManager::MipLevel* mip = font->getMipLevelForSize(static_cast<int>(targetPixelHeight));
	if (!mip) {
		SDL_SetRenderTarget(renderer, oldTarget);
		return;
	}
	const float mipRelativeScale = (mip->height > 0) ? (targetPixelHeight / mip->height) : 1.0f;
	SDL_Texture* tex = mip->fillTexture;

	for (int rowIndex = 0; rowIndex < rowsToActuallyRender; ++rowIndex) {
		float y = (drawableHeight + rowPadding) * rowIndex; // This is the baseline y for this row.
		float x = 0.0f;
		for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
			size_t colIndex = visibleColumnIndices_[i];
			if (colIndex >= table.rows[rowIndex].size()) {
				x += cachedColumnWidths_[i] + paddingBetweenColumns;
				continue;
			}
			const std::string& cell = table.rows[rowIndex][colIndex];
			float cellWidth = static_cast<float>(font->getWidth(cell)) * scale;
			float xAligned = x + (cachedColumnWidths_[i] - cellWidth) / 2.0f;
			float penX = xAligned;
			for (char c : cell) {
				auto it = mip->glyphs.find(c);
				if (it != mip->glyphs.end()) {
					const auto& g = it->second;
					SDL_Rect src = g.rect;
					// CORRECTED: Use the row's 'y' directly. Do not recalculate per-glyph vertical alignment.
					SDL_FRect dst = { penX, y, g.rect.w * mipRelativeScale, g.rect.h * mipRelativeScale };
					SDL_RenderCopyF(renderer, tex, &src, &dst);
					penX += g.advance * mipRelativeScale;
				}
			}
			x += cachedColumnWidths_[i] + paddingBetweenColumns;
		}
	}
#ifndef NDEBUG
	SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red, opaque
	SDL_Rect outlineRect = { 0, 0, static_cast<int>(totalTableWidth) - 1, tableRowsTextureHeight_ - 1 };
	SDL_RenderDrawRect(renderer, &outlineRect);
#endif
	SDL_SetRenderTarget(renderer, oldTarget);
}