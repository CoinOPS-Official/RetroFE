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
	, lastSelectedItem_(nullptr)
	, highScoreTable_(nullptr)
	, intermediateTexture_(nullptr)
{
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



ReloadableHiscores::~ReloadableHiscores() = default;


bool ReloadableHiscores::update(float dt) {
	if (waitEndTime_ > 0.0f) {
		waitEndTime_ -= dt;
		if (waitEndTime_ <= 0.0f) {
			// Ready to start scrolling again
			currentPosition_ = 0.0f; // Start from the top
			needsRedraw_ = true;
			LOG_DEBUG("ReloadableHiscores", "Wait time ended. Starting scroll.");
		}
	}
	else if (waitStartTime_ > 0.0f) {
		waitStartTime_ -= dt;
	}
	else {
		if (highScoreTable_ && !highScoreTable_->tables.empty()) {
			// Ensure currentTableIndex_ is within bounds
			if (currentTableIndex_ >= highScoreTable_->tables.size()) {
				currentTableIndex_ = 0;
				LOG_WARNING("ReloadableHiscores", "currentTableIndex_ out of bounds. Resetting to 0.");
			}

			const HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];

			// Calculate scaling and positioning
			FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
			float scale = baseViewInfo.FontSize / static_cast<float>(font->getHeight());
			float drawableHeight = static_cast<float>(font->getAscent()) * scale;
			float rowPadding = baseRowPadding_ * drawableHeight;
			float paddingBetweenColumns = baseColumnPadding_ * drawableHeight;

			// Cache column widths and compute total table width
			cacheColumnWidths(font, scale, table, paddingBetweenColumns);
			float totalTableWidth = cachedTotalTableWidth_;

			// Adjust scale if the table width exceeds the viewable area
			if (totalTableWidth > baseViewInfo.Width) {
				float downScaleFactor = baseViewInfo.Width / totalTableWidth;
				drawableHeight *= downScaleFactor;
				rowPadding *= downScaleFactor;
			}

			// Calculate heights separately
			size_t rowsToRender = std::min(table.rows.size(), maxRows_);

			// Height of the optional title row
			float titleHeight = table.id.empty() ? 0.0f : (drawableHeight + rowPadding);

			// Height of the header row
			float headerHeight = drawableHeight + rowPadding;

			// Height of all visible rows
			float rowsHeight = (drawableHeight + rowPadding) * static_cast<float>(rowsToRender);

			// Total height of the table (title + header + rows)
			float totalTableHeight = titleHeight + headerHeight + rowsHeight;

			// Scroll completion height (when all rows have scrolled out, leaving the header/title if present)
			float scrollCompletionHeight = totalTableHeight - headerHeight - titleHeight;

			LOG_DEBUG("ReloadableHiscores", "Total Table Height: " + std::to_string(totalTableHeight));
			LOG_DEBUG("ReloadableHiscores", "Scroll Completion Height: " + std::to_string(scrollCompletionHeight));

			// Determine if scrolling is required
			bool needsScrolling = (totalTableHeight > baseViewInfo.Height);

			if (needsScrolling) {
				currentPosition_ += scrollingSpeed_ * dt;

				LOG_DEBUG("ReloadableHiscores", "Scrolling... Current Position: " + std::to_string(currentPosition_));

				needsRedraw_ = true; // Keep redrawing while scrolling

				// Reset scrolling when it completes
				if (currentPosition_ >= scrollCompletionHeight) {
					if (highScoreTable_->tables.size() > 1) {
						// Switch to next table
						currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_->tables.size();
						reloadTexture(); // Refresh the texture for the new table
						needsRedraw_ = true;
						waitEndTime_ = startTime_; // Pause before scrolling starts again
						currentPosition_ = 0.0f; // Ensure the table is fully visible during the wait
						tableDisplayTimer_ = 0.0f;
						LOG_INFO("ReloadableHiscores", "Switched to table index: " + std::to_string(currentTableIndex_));
					}
					else {
						// Single table: reset scroll
						currentPosition_ = 0.0f; // Ensure the table is fully visible
						waitEndTime_ = startTime_; // Pause before scrolling starts again
						needsRedraw_ = true;
						LOG_INFO("ReloadableHiscores", "Scroll reset for single table.");
					}
				}
			}
			else {
				currentPosition_ = 0.0f; // Ensure non-scrolling tables remain visible
				needsRedraw_ = true;
			}

			if (highScoreTable_->tables.size() > 1) {
				// Update tableDisplayTimer_ only for multi-tables and scrolling tables
				if (needsScrolling) {
					currentTableDisplayTime_ = scrollCompletionHeight / scrollingSpeed_;
				}
				else {
					currentTableDisplayTime_ = displayTime_;
				}

				tableDisplayTimer_ += dt;

				LOG_DEBUG("ReloadableHiscores", "Table Display Timer: " + std::to_string(tableDisplayTimer_) + " / " + std::to_string(currentTableDisplayTime_));
			}
		}
	}

	if (newItemSelected || (newScrollItemSelected && getMenuScrollReload())) {
		LOG_INFO("ReloadableHiscores", "New item selected. Resetting table index to 0.");
		currentTableIndex_ = 0;
		tableDisplayTimer_ = 0.0f;  // Reset timer for the new game's table
		currentPosition_ = 0.0f;     // Reset scroll position
		reloadTexture(true);
		newItemSelected = false;
		newScrollItemSelected = false;
	}

	return Component::update(dt);
}
void ReloadableHiscores::allocateGraphicsMemory()
{
	Component::allocateGraphicsMemory();
	reloadTexture();
}


void ReloadableHiscores::freeGraphicsMemory()
{
	Component::freeGraphicsMemory();
	if (intermediateTexture_) {
		SDL_DestroyTexture(intermediateTexture_);
		intermediateTexture_ = nullptr;
	}
}


void ReloadableHiscores::deInitializeFonts()
{
	fontInst_->deInitialize();
}


void ReloadableHiscores::initializeFonts()
{
	fontInst_->initialize();
}


void ReloadableHiscores::reloadTexture(bool resetScroll) {
	if (resetScroll) {
		currentPosition_ = 0.0f; // Start scrolling from the defined start position
		waitStartTime_ = startTime_;
		waitEndTime_ = 0.0f;
	}

	Item* selectedItem = page.getSelectedItem(displayOffset_);
	if (selectedItem != lastSelectedItem_) {
		lastSelectedItem_ = selectedItem;
		if (selectedItem) {
			highScoreTable_ = HiScores::getInstance().getHighScoreTable(selectedItem->name);
			if (highScoreTable_ && !highScoreTable_->tables.empty()) {
				currentTableIndex_ = 0; // Reset to first table if a new high score table is selected
				const HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];
				updateVisibleColumns(table);
				LOG_INFO("ReloadableHiscores", "Loaded table index: " + std::to_string(currentTableIndex_));
			}
		}
		else {
			highScoreTable_ = nullptr;
			LOG_WARNING("ReloadableHiscores", "No high score table available for the selected item.");
		}
		cachedTableIndex_ = std::numeric_limits<size_t>::max(); // Invalidate column widths cache
		cacheValid_ = false;
	}

	needsRedraw_ = true; // Ensure the texture is redrawn after reload
}

void ReloadableHiscores::draw() {
	Component::draw();

	// Early exit conditions
	if (!(highScoreTable_ && !highScoreTable_->tables.empty()) ||
		baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	// Check if the table requires a redraw
	HighScoreTable& table = highScoreTable_->tables[currentTableIndex_];
	if (table.forceRedraw) {
		needsRedraw_ = true;
		cacheValid_ = false;
		table.forceRedraw = false;  // Reset the flag directly
	}

	// Retrieve font and texture
	FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
	SDL_Texture* texture = font ? font->getTexture() : nullptr;
	if (!texture) {
		LOG_ERROR("ReloadableHiscores", "Font texture is null.");
		return;
	}

	// Retrieve renderer
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	if (!renderer) {
		LOG_ERROR("ReloadableHiscores", "Unable to retrieve SDL_Renderer.");
		return;
	}


	// ======== Start of "hiscores" Rendering with Intermediate Texture ========
	float imageMaxWidth = (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
		? baseViewInfo.Width : baseViewInfo.MaxWidth;
	float imageMaxHeight = (baseViewInfo.Height < baseViewInfo.MaxHeight && baseViewInfo.Height > 0)
		? baseViewInfo.Height : baseViewInfo.MaxHeight;
	// Step 1: Save the current render target
	SDL_Texture* originalTarget = SDL_GetRenderTarget(renderer);
	if (!originalTarget) {
		LOG_ERROR("ReloadableHiscores", "Unable to get current render target.");
		return;
	}

	// Step 2: Create intermediate texture
	if (!intermediateTexture_) {
		if (!createIntermediateTexture(renderer, static_cast<int>(imageMaxWidth), static_cast<int>(imageMaxHeight))) {
			LOG_ERROR("ReloadableHiscores", "Failed to create intermediate texture.");
			return;
		}
	}
	if (needsRedraw_) {
		LOG_DEBUG("ReloadableHiscores", "Redraw triggered due to scrolling or table switch.");
		float scale = baseViewInfo.FontSize / static_cast<float>(font->getHeight());
		float drawableHeight = static_cast<float>(font->getAscent()) * scale;
		float rowPadding = baseRowPadding_ * drawableHeight;
		float paddingBetweenColumns = baseColumnPadding_ * drawableHeight;

		// Cache column widths and total table width
		cacheColumnWidths(font, scale, table, paddingBetweenColumns);
		float totalTableWidth = cachedTotalTableWidth_;
		float downScaleFactor = 1.0f;
		// Downscale if the table width exceeds the viewable area
		if (imageMaxWidth < totalTableWidth) {
			downScaleFactor = (imageMaxWidth / totalTableWidth) * 0.9f;
			scale *= downScaleFactor;
			drawableHeight *= downScaleFactor;
			rowPadding *= downScaleFactor;
			paddingBetweenColumns *= downScaleFactor;

			// Invalidate cache due to resizing
			cacheValid_ = false;

			// Recalculate column widths and table width
			cacheColumnWidths(font, scale, table, paddingBetweenColumns);
			totalTableWidth = cachedTotalTableWidth_;

		}

		float scrollOffset = currentPosition_;

		// Recalculate origin for centering
		float xOrigin = baseViewInfo.XRelativeToOrigin() + (imageMaxWidth - totalTableWidth) / 2.0f;
		float yOrigin = baseViewInfo.YRelativeToOrigin();

		// Set clipping rectangle
		SDL_Rect clipRect = { static_cast<int>(xOrigin), static_cast<int>(yOrigin),
			static_cast<int>(std::min(totalTableWidth, imageMaxWidth)),
			static_cast<int>(imageMaxHeight) };
		SDL_RenderSetClipRect(renderer, &clipRect);

		// Clear the intermediate texture
		SDL_SetRenderTarget(renderer, intermediateTexture_);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
		SDL_RenderClear(renderer);

		// Render Title, Headers, and Rows (centered using xOrigin and recalculated widths)
		float adjustedYOrigin = yOrigin;

		// Render title
		if (!table.id.empty()) {
			const std::string& title = table.id;
			float titleWidth = static_cast<float>(font->getWidth(title)) * scale;
			float titleX = xOrigin + (std::min(imageMaxWidth, totalTableWidth) - titleWidth) / 2.0f;
			float titleY = adjustedYOrigin;

			for (char c : title) {
				FontManager::GlyphInfo glyph;
				if (font->getRect(c, glyph)) {
					SDL_Rect srcRect = glyph.rect;
					SDL_FRect destRect = {
						titleX,
						titleY,
						glyph.rect.w * scale,
						glyph.rect.h * scale
					};
					SDL_RenderCopyF(renderer, texture, &srcRect, &destRect);
					titleX += static_cast<float>(glyph.advance) * scale;
				}
			}
			adjustedYOrigin += drawableHeight + rowPadding;
		}

		// Render headers
		float headerY = adjustedYOrigin;
		float xPos = xOrigin;
		for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
			size_t colIndex = visibleColumnIndices_[i];
			const std::string& header = table.columns[colIndex];
			float headerWidth = static_cast<float>(font->getWidth(header)) * scale;
			float xAligned = xPos + (cachedColumnWidths_[i] - headerWidth) / 2.0f;

			float charX = xAligned;
			for (char c : header) {
				FontManager::GlyphInfo glyph;
				if (font->getRect(c, glyph)) {
					SDL_Rect srcRect = glyph.rect;
					SDL_FRect destRect = {
						charX,
						headerY,
						glyph.rect.w * scale,
						glyph.rect.h * scale
					};
					SDL_RenderCopyF(renderer, texture, &srcRect, &destRect);
					charX += static_cast<float>(glyph.advance) * scale;
				}
			}
			xPos += cachedColumnWidths_[i] + paddingBetweenColumns;
		}

		adjustedYOrigin += drawableHeight + rowPadding;
		// Calculate base position for all rows
		float baseY = adjustedYOrigin - scrollOffset;

		// Adjusted scrollClipRect calculation
		SDL_Rect scrollClipRect = {
			static_cast<int>(xOrigin),
			static_cast<int>(adjustedYOrigin),
			static_cast<int>(std::min(totalTableWidth, imageMaxWidth)),
			static_cast<int>(std::max(imageMaxHeight - (adjustedYOrigin - yOrigin), 0.0f))
		};
		SDL_RenderSetClipRect(renderer, &scrollClipRect);

		// Render rows
		size_t renderedRows = 0; // Track the number of rows rendered
		for (size_t rowIndex = 0; rowIndex < table.rows.size(); ++rowIndex) {
			// Stop rendering if the number of rendered rows reaches the maxRows_ limit
			if (renderedRows >= maxRows_) {
				break;
			}

			float currentY = baseY + static_cast<float>(rowIndex) * (drawableHeight + rowPadding);

			float rowTop = currentY;
			float rowBottom = currentY + drawableHeight;

			if (rowBottom < yOrigin || rowTop > yOrigin + imageMaxHeight) {
				continue;  // Skip rows fully outside the visible area
			}

			xPos = xOrigin;
			for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
				size_t colIndex = visibleColumnIndices_[i];
				if (colIndex >= table.rows[rowIndex].size()) continue;

				const std::string& cell = table.rows[rowIndex][colIndex];
				float cellWidth = static_cast<float>(font->getWidth(cell)) * scale;
				float xAligned = xPos + (cachedColumnWidths_[i] - cellWidth) / 2.0f;

				float charX = xAligned;
				for (char c : cell) {
					FontManager::GlyphInfo glyph;
					if (font->getRect(c, glyph)) {
						SDL_Rect srcRect = glyph.rect;
						SDL_FRect destRect = {
							charX,
							currentY,
							glyph.rect.w * scale,
							glyph.rect.h * scale
						};
						SDL_RenderCopyF(renderer, texture, &srcRect, &destRect);
						charX += static_cast<float>(glyph.advance) * scale;
					}
				}
				xPos += cachedColumnWidths_[i] + paddingBetweenColumns;
			}

			++renderedRows; // Increment the rendered rows count
		}

		SDL_RenderSetClipRect(renderer, nullptr);
		SDL_SetRenderTarget(renderer, originalTarget);
	}

	// Step 6: Define the destination rectangle where the intermediate texture should be drawn
	SDL_FRect destRect = {
		baseViewInfo.XOrigin,
		baseViewInfo.YOrigin,
		baseViewInfo.Width,
		baseViewInfo.Height
	};

	// Step 7: Render the intermediate texture to the original render target
	SDL::renderCopyF(intermediateTexture_, baseViewInfo.Alpha, nullptr, &destRect, baseViewInfo,
		page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
		page.getLayoutHeightByMonitor(baseViewInfo.Monitor));

	needsRedraw_ = false;
}

void ReloadableHiscores::cacheColumnWidths(FontManager* font, float scale, const HighScoreTable& table, float paddingBetweenColumns) {
	// Return early if the table hasn't changed and scale/padding are the same
	if (cacheValid_ && currentTableIndex_ == cachedTableIndex_ &&
		lastScale_ == scale && lastPaddingBetweenColumns_ == paddingBetweenColumns) {
		return; // Cache is valid, no need to recalculate
	}

	// Update cached scale and padding
	lastScale_ = scale;
	lastPaddingBetweenColumns_ = paddingBetweenColumns;

	// Initialize or reset cached column widths and total width
	cachedColumnWidths_.clear();
	cachedTotalTableWidth_ = 0.0f;

	// Iterate over visible columns
	for (size_t visibleIndex = 0; visibleIndex < visibleColumnIndices_.size(); ++visibleIndex) {
		size_t colIndex = visibleColumnIndices_[visibleIndex];
		float maxColumnWidth = 0.0f;

		// Compare header width
		if (colIndex < table.columns.size()) {
			maxColumnWidth = std::max(maxColumnWidth, static_cast<float>(font->getWidth(table.columns[colIndex])) * scale);
		}

		// Compare all rows' cell widths
		for (const auto& row : table.rows) {
			if (colIndex < row.size()) {
				float cellWidth = static_cast<float>(font->getWidth(row[colIndex])) * scale;
				maxColumnWidth = std::max(maxColumnWidth, cellWidth);
			}
		}

		// Store the maximum width for the current column
		cachedColumnWidths_.push_back(maxColumnWidth);

		// Update total table width
		cachedTotalTableWidth_ += maxColumnWidth + paddingBetweenColumns;
	}

	// Adjust total width by subtracting the last padding
	if (!cachedColumnWidths_.empty()) {
		cachedTotalTableWidth_ -= paddingBetweenColumns; // Remove extra padding after the last column
	}

	// Debugging log for column widths and total table width
	LOG_DEBUG("ReloadableHiscores", "Cached Column Widths: ");
	for (const auto& width : cachedColumnWidths_) {
		LOG_DEBUG("Column Width", std::to_string(width));
	}
	LOG_DEBUG("Cached Total Table Width", std::to_string(cachedTotalTableWidth_));

	cacheValid_ = true;
	// Update the cached table index
	cachedTableIndex_ = currentTableIndex_;
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


bool ReloadableHiscores::createIntermediateTexture(SDL_Renderer* renderer, int width, int height) {
	// Destroy existing texture if it exists
	if (intermediateTexture_) {
		SDL_DestroyTexture(intermediateTexture_);
		intermediateTexture_ = nullptr;
	}

	// Create the intermediate texture with alpha support
	intermediateTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
	if (!intermediateTexture_) {
		LOG_ERROR("ReloadableHiscores", "Failed to create intermediate texture: " + std::string(SDL_GetError()));
		return false;
	}

	// Set the blend mode to allow transparency
	if (SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND) != 0) {
		LOG_ERROR("ReloadableHiscores", "Failed to set blend mode for intermediate texture: " + std::string(SDL_GetError()));
		SDL_DestroyTexture(intermediateTexture_);
		intermediateTexture_ = nullptr;
		return false;
	}

	return true;
}


