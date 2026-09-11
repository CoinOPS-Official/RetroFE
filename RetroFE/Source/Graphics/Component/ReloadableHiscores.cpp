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
#include "../../Database/HighScoreChange.h"
#include "../../Database/LocalHiScores.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include "../Font.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <numeric>
#include <string_view>
#include <cmath>

namespace {
	constexpr float kNoDataHoldSeconds = 3.0f;
	constexpr float kNoDataFadeSeconds = 0.6f;

	LocalScoreQuery localScoreQuery(const Item* item) {
		if (!item) return {};
		return {
			item->name,
			item->mameMachine,
			item->mameSoftwareList,
			item->mameSoftware
		};
	}

	static float measureTextWidthExact(FontManager* f, const std::string& s, float scale) {
		if (!f || s.empty()) return 0.0f;
		const float targetH = scale * f->getMaxHeight();
		const FontManager::MipLevel* mip = f->getMipLevelForSize((int)targetH);
		if (!mip || !mip->fillTexture) {
			return (float)f->getWidth(s) * scale;
		}
		const float k = (mip->height > 0) ? (targetH / mip->height) : 1.0f;
		const bool hasOutline = (mip->outlineTexture != nullptr);

		float penX = 0.0f, minX = 0.0f, maxX = 0.0f;
		bool first = true;
		Uint32 prev = 0;

		const char* ptr = s.c_str();
		const char* end = ptr + s.size();

		while (ptr < end) {
			uint32_t codepoint = 0;
			unsigned char c = static_cast<unsigned char>(*ptr++);

			if (c < 0x80) { codepoint = c; }
			else if ((c & 0xE0) == 0xC0) {
				if (ptr + 1 > end) break;
				codepoint = ((c & 0x1F) << 6) | (static_cast<unsigned char>(*ptr++) & 0x3F);
			}
			else if ((c & 0xF0) == 0xE0) {
				if (ptr + 2 > end) break;
				unsigned char b1 = static_cast<unsigned char>(*ptr++);
				unsigned char b2 = static_cast<unsigned char>(*ptr++);
				codepoint = ((c & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
			}
			else if ((c & 0xF8) == 0xF0) {
				if (ptr + 3 > end) break;
				unsigned char b1 = static_cast<unsigned char>(*ptr++);
				unsigned char b2 = static_cast<unsigned char>(*ptr++);
				unsigned char b3 = static_cast<unsigned char>(*ptr++);
				codepoint = ((c & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
			}
			else { prev = 0; continue; }

			const Uint32 ch = codepoint;
			if (prev) penX += f->getKerning(prev, ch) * scale;

			// FIXED: Safe multi-map lookup using pointers instead of cross-container iterators
			const FontManager::GlyphInfo* g = nullptr;
			auto it = mip->glyphs.find(ch);
			if (it != mip->glyphs.end()) {
				g = &it->second;
			}
			else {
				auto itDyn = mip->dynamicGlyphs.find(ch);
				if (itDyn != mip->dynamicGlyphs.end()) {
					g = &itDyn->second;
				}
			}

			if (g) {
				float left = penX;
				float right = penX + g->fillW * k;

				if (hasOutline) {
					const float oL = penX - g->fillX * k;
					const float oR = oL + g->rect.w * k;
					left = std::min(left, oL);
					right = std::max(right, oR);
				}

				if (first) {
					minX = left; maxX = right;
					first = false;
				}
				else {
					minX = std::min(minX, left);
					maxX = std::max(maxX, right);
				}
				penX += g->advance * k;
			}
			prev = ch;
		}

		return std::max(0.0f, maxX - minX);
	}

	static void renderTextOutlined(
		SDL_Renderer* r,
		FontManager* f,
		const FontManager::MipLevel* mip,
		SDL_Texture* staticFillTex,
		SDL_Texture* staticOutlineTex,
		const std::string& s,
		float x,
		float y,
		float finalScale,
		float k) {
		if (!r || !f || !mip || !staticFillTex || s.empty()) return;

		const float ySnap = std::round(y);

		// Helper to perform a single pass (Outline or Fill)
		auto doPass = [&](SDL_Texture* staticTex, SDL_Texture* dynamicTex, bool isFill) {
			if (!staticTex && !dynamicTex) return;

			float penX = x;
			Uint32 prev = 0;
			const char* ptr = s.c_str();
			const char* end = ptr + s.size();

			while (ptr < end) {
				uint32_t codepoint = 0;
				unsigned char c = static_cast<unsigned char>(*ptr++);

				if (c < 0x80) { codepoint = c; }
				else if ((c & 0xE0) == 0xC0) {
					if (ptr + 1 > end) break;
					codepoint = ((c & 0x1F) << 6) | (static_cast<unsigned char>(*ptr++) & 0x3F);
				}
				else if ((c & 0xF0) == 0xE0) {
					if (ptr + 2 > end) break;
					unsigned char b1 = static_cast<unsigned char>(*ptr++);
					unsigned char b2 = static_cast<unsigned char>(*ptr++);
					codepoint = ((c & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
				}
				else if ((c & 0xF8) == 0xF0) {
					if (ptr + 3 > end) break;
					unsigned char b1 = static_cast<unsigned char>(*ptr++);
					unsigned char b2 = static_cast<unsigned char>(*ptr++);
					unsigned char b3 = static_cast<unsigned char>(*ptr++);
					codepoint = ((c & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
				}
				else { prev = 0; continue; }

				const Uint32 ch = codepoint;
				if (prev) penX += f->getKerning(prev, ch) * finalScale;

				// FIXED: Safe multi-map lookup
				const FontManager::GlyphInfo* g = nullptr;
				SDL_Texture* texToUse = nullptr;

				auto it = mip->glyphs.find(ch);
				if (it != mip->glyphs.end()) {
					g = &it->second;
					texToUse = staticTex;
				}
				else {
					auto itDyn = mip->dynamicGlyphs.find(ch);
					if (itDyn != mip->dynamicGlyphs.end()) {
						g = &itDyn->second;
						texToUse = dynamicTex;
					}
				}

				if (g && texToUse) {
					if (isFill) {
						SDL_Rect srcFill{ g->rect.x + g->fillX, g->rect.y + g->fillY, g->fillW, g->fillH };
						SDL_FRect dstFill{ penX, ySnap, g->fillW * k, g->fillH * k };
						SDL_RenderCopyF(r, texToUse, &srcFill, &dstFill);
					}
					else {
						const SDL_Rect& src = g->rect;
						SDL_FRect dst = { penX - g->fillX * k, ySnap - g->fillY * k, src.w * k, src.h * k };
						SDL_RenderCopyF(r, texToUse, &src, &dst);
					}
					penX += g->advance * k;
				}
				prev = ch;
			}
			};

		// --- Outline pass ---
		doPass(staticOutlineTex, mip->dynamicOutlineTexture, false);

		// --- Fill pass ---
		doPass(staticFillTex, mip->dynamicFillTexture, true);
	}

} // namespace

ReloadableHiscores::ReloadableHiscores(Configuration& config, std::string textFormat,
	Page& p, int displayOffset, FontManager* font, float scrollingSpeed, float startTime,
	std::string excludedColumns, float baseColumnPadding, float baseRowPadding, size_t maxRows)
	: Component(p)
	, config_(config)
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
	, tableCrossfading_(false)
	, tableCrossfadeTimer_(0.0f)
	, tableCrossfadeDuration_(0.35f)
	, showingNoData_(false)
	, noDataElapsed_(0.0f)
	, wasComponentVisible_(false)
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
	, lastSelectedGame_()
	, lastRenderedRevision_(0)
	, highScoreTable_()
	, headerTexture_(nullptr)
	, tableRowsTexture_(nullptr)
	, previousTableTexture_(nullptr)
	, compositeTexture_(nullptr)
	, compositeTextureWidth_(0)
	, compositeTextureHeight_(0)
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
	return updatePages_(dt);
#if 0
	if (tableCrossfading_) {
		tableCrossfadeTimer_ += dt;
		needsRedraw_ = true;
		if (tableCrossfadeTimer_ >= tableCrossfadeDuration_) cancelTableTransition_();
	}

	Item* selectedItem = page.getSelectedItem(displayOffset_);
	if (selectedItem && selectedItem == lastSelectedItem_ &&
		!(newItemSelected || (newScrollItemSelected && getMenuScrollReload()))) {
		const uint64_t revision = LocalHiScores::getInstance().getRevision(localScoreQuery(selectedItem));
		if (revision != lastRenderedRevision_) {
			cancelTableTransition_();
			HighScoreSnapshot snapshot = LocalHiScores::getInstance().getTable(localScoreQuery(selectedItem));
			LOG_INFO("ReloadableHiscores", "High score redraw requested for " + selectedItem->name + ".");
			highScoreTable_ = std::move(snapshot.view);
			lastRenderedRevision_ = snapshot.revision;
			currentTableIndex_ = 0;
			tableDisplayTimer_ = 0.0f;
			cacheValid_ = false;
			reloadTexture(true);
		}
	}

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
			if (!highScoreTable_.tables.empty() &&
				cachedTableIndex_ != currentTableIndex_ && cacheValid_) {
				resetScrollForParamReload = true;
			}
			reloadTexture(resetScrollForParamReload);
			// reloadTexture sets cacheValid_ = true if successful for a table
		}

		// --- 2. Scrolling and Table Switching Logic ---
		if (!highScoreTable_.tables.empty()) {
			// Ensure currentTableIndex_ is within bounds before use
			if (currentTableIndex_ >= highScoreTable_.tables.size()) {
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
				const HighScoreTableView& table = highScoreTable_.tables[currentTableIndex_];

				// Use authoritative geometric values from cache
				float drawableHeight = lastComputedDrawableHeight_;
				float rowPadding = lastComputedRowPadding_;
				size_t rowsToRender = std::min(table.rows.size(), maxRows_);

				// 1. Calculate the total number of rows with content
				int totalRows = 0;
				if (!table.id.empty()) totalRows++;      // Title row
				totalRows++;                             // Column header row (assumed always present)
				totalRows += static_cast<int>(rowsToRender);

				// 2. Define the Trigger Height (The "Visible Footprint")
				// We only count the gaps BETWEEN rows. This prevents 
				// scrolling if the table fits perfectly to the last pixel of text.
				float visibleFootprint = (totalRows > 0)
					? (drawableHeight * totalRows) + (rowPadding * (totalRows - 1))
					: 0.0f;

				// 3. Define the Completion Target
				// We keep the trailing padding here so the scroll finishes 
				// cleanly by moving the entire block (including its bottom margin) out of view.
				float scrollCompletionTarget = std::max(0.0f,
					static_cast<float>(tableRowsTextureHeight_) -
					std::max(0.0f, baseViewInfo.ScaledHeight() - headerTextureHeight_));

				// Trigger scrolling ONLY if the ink/text footprint exceeds the view
				bool needsScrolling = (visibleFootprint > baseViewInfo.Height);

				if (needsScrolling) {
					currentPosition_ += scrollingSpeed_ * dt;
					needsRedraw_ = true;

					if (currentPosition_ >= scrollCompletionTarget) {
						if (highScoreTable_.tables.size() > 1) {
							beginTableTransition_();
							currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_.tables.size();
							waitEndTime_ = startTime_;
							currentPosition_ = 0.0f;
							tableDisplayTimer_ = 0.0f;
							reloadTexture(true);
							LOG_INFO("ReloadableHiscores", "Switched table (scrolling) to index: " + std::to_string(currentTableIndex_));
						}
						else {
							currentPosition_ = 0.0f;
							waitEndTime_ = startTime_;
							needsRedraw_ = true;
							LOG_INFO("ReloadableHiscores", "Scroll reset for single table.");
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
					if (highScoreTable_.tables.size() > 1) {
						currentTableDisplayTime_ = displayTime_; // Static display time
						tableDisplayTimer_ += dt;

						// LOG_DEBUG("ReloadableHiscores", "Static Table Display Timer: " + std::to_string(tableDisplayTimer_) + " / " + std::to_string(currentTableDisplayTime_));

						if (tableDisplayTimer_ >= currentTableDisplayTime_) {
							beginTableTransition_();
							currentTableIndex_ = (currentTableIndex_ + 1) % highScoreTable_.tables.size();
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
			else if (!highScoreTable_.tables.empty()) {
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
		cancelTableTransition_();
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
#endif
}

void ReloadableHiscores::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	rebuildPagePlan_();
	buildCurrentPage_();
}


void ReloadableHiscores::freeGraphicsMemory() {
	Component::freeGraphicsMemory();
	lastSelectedItem_ = nullptr;
	lastSelectedGame_.clear();
	if (headerTexture_) { SDL_DestroyTexture(headerTexture_); headerTexture_ = nullptr; }
	if (tableRowsTexture_) { SDL_DestroyTexture(tableRowsTexture_); tableRowsTexture_ = nullptr; }
	if (previousTableTexture_) { SDL_DestroyTexture(previousTableTexture_); previousTableTexture_ = nullptr; }
	if (compositeTexture_) { SDL_DestroyTexture(compositeTexture_); compositeTexture_ = nullptr; }
	compositeTextureWidth_ = 0;
	compositeTextureHeight_ = 0;
	freePagePanels_();
	tableCrossfading_ = false;
	wholePageTransition_ = false;
	previousPanelStates_.clear();
	transitioningTableIndices_.clear();
	showingNoData_ = false;
	noDataElapsed_ = 0.0f;
	wasComponentVisible_ = false;
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
	const std::string selectedGame = selectedItem ? selectedItem->name : std::string();
	bool itemChanged = (selectedItem != lastSelectedItem_) || (selectedGame != lastSelectedGame_);

	if (itemChanged) {
		lastSelectedItem_ = selectedItem;
		lastSelectedGame_ = selectedGame;
		if (selectedItem) {
			HighScoreSnapshot snapshot = LocalHiScores::getInstance().getTable(localScoreQuery(selectedItem));
			highScoreTable_ = std::move(snapshot.view);
			lastRenderedRevision_ = snapshot.revision;
			if (!highScoreTable_.tables.empty()) currentTableIndex_ = 0;
		}
		else {
			highScoreTable_.tables.clear();
			lastRenderedRevision_ = 0;
		}
	}

	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
	if (!renderer || !font || font->getMaxHeight() <= 0) return;

	// --- Path A: No Scores ---
	if (highScoreTable_.tables.empty()) {
		renderNoDataMessage(renderer, font); // Consolidate your message logic here
		return;
	}

	// --- Path B: Normal Rendering ---
	const HighScoreTableView& table = highScoreTable_.tables[currentTableIndex_];
	if (itemChanged || cachedTableIndex_ != currentTableIndex_) {
		updateVisibleColumns(table);
	}

	float effectiveViewWidth = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
		? baseViewInfo.Width : baseViewInfo.MaxWidth;

	std::vector<float> colWidths;
	float totalTableWidth = 0, drawableHeight, rowPadding, paddingBetweenColumns;

	float finalScale = computeTableScaleAndWidths(
		font, table, drawableHeight, rowPadding, paddingBetweenColumns,
		colWidths, totalTableWidth, effectiveViewWidth, baseViewInfo.ScaledHeight());

	// Update authoritative cache
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

	// --- Optimized Texture Allocation (The Allocation Guard) ---
	int targetW = static_cast<int>(std::ceil(totalTableWidth));
	int targetHeaderH = static_cast<int>(drawableHeight + rowPadding) * (table.id.empty() ? 1 : 2);

	size_t rowsToRender = std::min(table.rows.size(), maxRows_);
	int targetRowsH = static_cast<int>((drawableHeight + rowPadding) * rowsToRender);
	if (targetRowsH <= 0) targetRowsH = 1;

	// Only recreate textures if the size has changed. This prevents VRAM thrashing.
	auto manageTexture = [&](SDL_Texture*& tex, int w, int h, int& currentH) {
		int actualW, actualH;
		bool sizeMismatch = true;
		if (tex) {
			SDL_QueryTexture(tex, nullptr, nullptr, &actualW, &actualH);
			if (actualW == w && actualH == h) sizeMismatch = false;
		}

		if (sizeMismatch) {
			if (tex) SDL_DestroyTexture(tex);
			tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
			if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
		}
		currentH = h;
		};

	// Use currentHeaderH_ and currentRowsH_ members to track heights across calls
	manageTexture(headerTexture_, targetW, targetHeaderH, headerTextureHeight_);
	manageTexture(tableRowsTexture_, targetW, targetRowsH, tableRowsTextureHeight_);

	// --- Consolidated Render Pass ---
	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);

	// Mipmapping Setup
	const float targetPixelHeight = finalScale * font->getMaxHeight();
	const FontManager::MipLevel* mip = font->getMipLevelForSize(static_cast<int>(targetPixelHeight));
	if (mip) {
		const float mipRelativeScale = (mip->height > 0) ? (targetPixelHeight / mip->height) : 1.0f;
		SDL_Texture* fillTex = mip->fillTexture;
		SDL_Texture* outlineTex = mip->outlineTexture;

		SDL_SetTextureColorMod(fillTex, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
		if (mip->dynamicFillTexture) {
			SDL_SetTextureColorMod(mip->dynamicFillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
		}

		// Draw Header
		if (headerTexture_) {
			SDL_SetRenderTarget(renderer, headerTexture_);
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
			SDL_RenderClear(renderer);

			float y = 0.0f;
			if (!table.id.empty()) {
				float titleW = measureTextWidthExact(font, table.id, finalScale);
				renderTextOutlined(renderer, font, mip, fillTex, outlineTex, table.id, (totalTableWidth - titleW) / 2.0f, y, finalScale, mipRelativeScale);
				y += drawableHeight + rowPadding;
			}

			float columnsWidth = std::accumulate(cachedColumnWidths_.begin(), cachedColumnWidths_.end(), 0.0f);
			if (cachedColumnWidths_.size() > 1)
				columnsWidth += paddingBetweenColumns * static_cast<float>(cachedColumnWidths_.size() - 1);
			float x = std::max(0.0f, (totalTableWidth - columnsWidth) * 0.5f);
			for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
				const std::string& hText = table.columns[visibleColumnIndices_[i]];
				float hW = measureTextWidthExact(font, hText, finalScale);
				renderTextOutlined(renderer, font, mip, fillTex, outlineTex, hText, x + (cachedColumnWidths_[i] - hW) / 2.0f, y, finalScale, mipRelativeScale);
				x += cachedColumnWidths_[i] + paddingBetweenColumns;
			}
		}

		// Draw Rows
		if (tableRowsTexture_) {
			SDL_SetRenderTarget(renderer, tableRowsTexture_);
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
			SDL_RenderClear(renderer);

			for (size_t r = 0; r < rowsToRender; ++r) {
				float y = (drawableHeight + rowPadding) * r;
				float columnsWidth = std::accumulate(cachedColumnWidths_.begin(), cachedColumnWidths_.end(), 0.0f);
				if (cachedColumnWidths_.size() > 1)
					columnsWidth += paddingBetweenColumns * static_cast<float>(cachedColumnWidths_.size() - 1);
				float x = std::max(0.0f, (totalTableWidth - columnsWidth) * 0.5f);
				for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
					size_t colIdx = visibleColumnIndices_[i];
					if (colIdx < table.rows[r].size()) {
						const std::string& cell = table.rows[r][colIdx];
						float cW = measureTextWidthExact(font, cell, finalScale);
						renderTextOutlined(renderer, font, mip, fillTex, outlineTex, cell, x + (cachedColumnWidths_[i] - cW) / 2.0f, y, finalScale, mipRelativeScale);
					}
					x += cachedColumnWidths_[i] + paddingBetweenColumns;
				}
			}
		}
	}

	SDL_SetRenderTarget(renderer, oldTarget);
	needsRedraw_ = true;
}

void ReloadableHiscores::renderNoDataMessage(SDL_Renderer* renderer, FontManager* font) {
	// 1. Cleanup: We definitely don't need the rows texture for a static message
	if (tableRowsTexture_) {
		SDL_DestroyTexture(tableRowsTexture_);
		tableRowsTexture_ = nullptr;
	}
	tableRowsTextureHeight_ = 0;

	// 2. Determine component dimensions for centering
	float viewW = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
		? baseViewInfo.Width : baseViewInfo.MaxWidth;
	float viewH = (baseViewInfo.Height > 0) ? baseViewInfo.Height : 1.0f;

	// 3. Define the message
	const std::vector<std::string> lines = {
		"LOCAL SCORES NOT AVAILABLE",
		"OR NOT YET SUPPORTED",
		"FOR THIS GAME"
	};

	// 4. Calculate metrics (Matching your table's look)
	const float scale = baseViewInfo.FontSize / static_cast<float>(font->getMaxHeight());
	const float drawableHeight = font->getMaxAscent() * scale;
	const float rowPadding = std::max(1.0f, baseRowPadding_ * drawableHeight);

	const float totalTextHeight = (drawableHeight * lines.size()) + (rowPadding * (lines.size() - 1));
	float y = (viewH - totalTextHeight) * 0.5f;
	if (y < 0.0f) y = 0.0f;

	// 5. Allocation Guard: Reuse headerTexture_ if it's already the right size
	int targetW = static_cast<int>(std::ceil(viewW));
	int targetH = static_cast<int>(std::ceil(viewH));

	int currentW, currentH;
	bool needsNewTexture = true;
	if (headerTexture_) {
		SDL_QueryTexture(headerTexture_, nullptr, nullptr, &currentW, &currentH);
		if (currentW == targetW && currentH == targetH) needsNewTexture = false;
	}

	if (needsNewTexture) {
		if (headerTexture_) SDL_DestroyTexture(headerTexture_);
		headerTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, targetW, targetH);
		if (headerTexture_) SDL_SetTextureBlendMode(headerTexture_, SDL_BLENDMODE_BLEND);
	}

	if (!headerTexture_) return;

	// 6. Setup Mipmapping
	const float targetPixelHeight = scale * font->getMaxHeight();
	const FontManager::MipLevel* mip = font->getMipLevelForSize(static_cast<int>(targetPixelHeight));

	// 7. Render Pass
	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, headerTexture_);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);

	if (mip && mip->fillTexture) {
		const float mipRelativeScale = (mip->height > 0) ? (targetPixelHeight / mip->height) : 1.0f;

		SDL_SetTextureColorMod(mip->fillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
		if (mip->dynamicFillTexture) {
			SDL_SetTextureColorMod(mip->dynamicFillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
		}

		for (const auto& line : lines) {
			float textW = measureTextWidthExact(font, line, scale);
			float x = (viewW - textW) * 0.5f;
			if (x < 0.0f) x = 0.0f;

			renderTextOutlined(renderer, font, mip, mip->fillTexture, mip->outlineTexture,
				line, x, y, scale, mipRelativeScale);
			y += drawableHeight + rowPadding;
		}
	}

	// 8. Finalize State
	SDL_SetRenderTarget(renderer, oldTarget);

	headerTextureHeight_ = targetH;
	cachedTotalTableWidth_ = viewW;
	cacheValid_ = true;
	needsRedraw_ = true;
	showingNoData_ = true;
	noDataElapsed_ = 0.0f;
}

float ReloadableHiscores::measureNaturalWidth_(FontManager* font, const HighScoreTableView& table,
	const std::vector<size_t>& columns, float scale) const {
	const float pad = baseColumnPadding_ * font->getMaxHeight() * scale;
	float total = 0.0f;
	for (size_t i = 0; i < columns.size(); ++i) {
		const size_t c = columns[i];
		float widest = c < table.columns.size() ? measureTextWidthExact(font, table.columns[c], scale) : 0.0f;
		for (const auto& row : table.rows)
			if (c < row.size()) widest = std::max(widest, measureTextWidthExact(font, row[c], scale));
		total += widest + (i + 1 < columns.size() ? pad : 0.0f);
	}
	if (!table.id.empty()) total = std::max(total, measureTextWidthExact(font, table.id, scale));
	return total;
}

void ReloadableHiscores::freePagePanels_() {
	for (auto& panel : pagePanels_) {
		if (panel.header) SDL_DestroyTexture(panel.header);
		for (SDL_Texture* tile : panel.rowTiles) if (tile) SDL_DestroyTexture(tile);
	}
	pagePanels_.clear();
}

void ReloadableHiscores::rebuildPagePlan_() {
	pagePlan_.clear();
	FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
	const float fullW = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
		? baseViewInfo.Width : baseViewInfo.MaxWidth;
	cachedViewWidth_ = fullW;
	cachedViewHeight_ = baseViewInfo.ScaledHeight();
	cachedBaseFontSize_ = baseViewInfo.FontSize;
	if (!font || font->getMaxHeight() <= 0 || highScoreTable_.tables.empty()) return;

	const float gap = fullW * 0.04f;
	const float halfW = std::max(1.0f, (fullW - gap) * 0.5f);
	const float baseScale = baseViewInfo.FontSize / font->getMaxHeight();
	std::vector<LocalTableCandidate> candidates;
	candidates.reserve(highScoreTable_.tables.size());

	for (size_t ti = 0; ti < highScoreTable_.tables.size(); ++ti) {
		const auto& table = highScoreTable_.tables[ti];
		size_t maxCols = table.columns.size();
		for (const auto& row : table.rows) maxCols = std::max(maxCols, row.size());
		std::vector<size_t> cols;
		for (size_t c = 0; c < maxCols; ++c) {
			const std::string name = c < table.columns.size() ? Utils::toLower(table.columns[c]) : std::string();
			if (name.empty() || excludedColumnsSet_.find(name) == excludedColumnsSet_.end()) cols.push_back(c);
		}
		const float naturalW = measureNaturalWidth_(font, table, cols, baseScale);
		const size_t totalRows = std::min(table.rows.size(), maxRows_);
		const size_t visibleRows = std::min<size_t>(10, totalRows);
		const float baseGlyphH = font->getMaxHeight() * baseScale;
		const float basePadH = baseGlyphH * baseRowPadding_;
		const float targetLines = static_cast<float>(1 + (table.id.empty() ? 0 : 1) + visibleRows);
		const float targetH = targetLines * baseGlyphH + std::max(0.0f, targetLines - 0.5f) * basePadH;
		const float ratio = std::clamp(std::min(halfW / std::max(1.0f, naturalW),
			baseViewInfo.ScaledHeight() / std::max(1.0f, targetH)), 0.0f, 1.0f);
		candidates.push_back({ ti, baseViewInfo.FontSize * ratio, visibleRows, totalRows,
			!cols.empty() && (!table.rows.empty() || !table.columns.empty() || !table.id.empty()) });
	}
	pagePlan_ = buildLocalScorePagePlan(candidates);
	currentPageIndex_ = std::min(currentPageIndex_, pagePlan_.empty() ? size_t(0) : pagePlan_.size() - 1);
}

void ReloadableHiscores::buildCurrentPage_(bool resetPresentation) {
	freePagePanels_();
	showingNoData_ = false;
	noDataElapsed_ = 0.0f;
	if (pagePlan_.empty() || currentPageIndex_ >= pagePlan_.size()) return;
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
	if (!renderer || !font) return;

	const auto& plan = pagePlan_[currentPageIndex_];
	const bool paired = plan.tableIndices.size() == 2;
	const float fullW = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth) ? baseViewInfo.Width : baseViewInfo.MaxWidth;
	const float fullH = baseViewInfo.ScaledHeight();
	const float gap = paired ? fullW * 0.04f : 0.0f;
	const float cellW = paired ? (fullW - gap) * 0.5f : fullW;
	const float baseScale = baseViewInfo.FontSize / font->getMaxHeight();
	const bool reserveTitle = std::any_of(plan.tableIndices.begin(), plan.tableIndices.end(), [&](size_t i) { return !highScoreTable_.tables[i].id.empty(); });

	float sharedRatio = 1.0f;
	std::vector<std::vector<size_t>> pageCols;
	for (size_t ti : plan.tableIndices) {
		const auto& table = highScoreTable_.tables[ti];
		size_t maxCols = table.columns.size(); for (const auto& row : table.rows) maxCols = std::max(maxCols, row.size());
		std::vector<size_t> cols;
		for (size_t c = 0; c < maxCols; ++c) {
			const std::string name = c < table.columns.size() ? Utils::toLower(table.columns[c]) : std::string();
			if (name.empty() || excludedColumnsSet_.find(name) == excludedColumnsSet_.end()) cols.push_back(c);
		}
		pageCols.push_back(cols);
		const float naturalW = measureNaturalWidth_(font, table, cols, baseScale);
		const size_t targetRows = std::min<size_t>(10, std::min(table.rows.size(), maxRows_));
		const float baseGlyphH = font->getMaxHeight() * baseScale;
		const float basePadH = baseGlyphH * baseRowPadding_;
		const float targetLines = static_cast<float>(1 + (reserveTitle ? 1 : 0) + targetRows);
		const float targetH = targetLines * baseGlyphH + std::max(0.0f, targetLines - 0.5f) * basePadH;
		sharedRatio = std::min(sharedRatio, std::min(cellW / std::max(1.0f, naturalW), fullH / std::max(1.0f, targetH)));
	}
	sharedRatio = std::max(1.0f / 64.0f, std::floor(std::clamp(sharedRatio, 0.0f, 1.0f) * 64.0f) / 64.0f);

	for (size_t pi = 0; pi < plan.tableIndices.size(); ++pi) {
		PagePanel panel;
		panel.tableIndex = plan.tableIndices[pi]; panel.visibleColumns = pageCols[pi]; panel.width = cellW;
		panel.x = pi == 0 ? 0.0f : cellW + gap; panel.scale = baseScale * sharedRatio;
		panel.lineStep = font->getMaxHeight() * panel.scale * (1.0f + baseRowPadding_);
		panel.headerHeight = panel.lineStep * (1 + (reserveTitle ? 1 : 0));
		const auto& table = highScoreTable_.tables[panel.tableIndex];
		const size_t rowCount = std::min(table.rows.size(), maxRows_);
		const float glyphH = font->getMaxHeight() * panel.scale;
		const float padH = glyphH * baseRowPadding_;
		panel.rowsHeight = rowCount > 0
			? rowCount * glyphH + (static_cast<float>(rowCount) - 0.5f) * padH
			: 0.0f;
		panel.maxScroll = std::max(0.0f, panel.rowsHeight - std::max(0.0f, fullH - panel.headerHeight));
		SDL_RendererInfo rendererInfo{};
		if (SDL_GetRendererInfo(renderer, &rendererInfo) == 0 && rendererInfo.max_texture_height > 0) {
			panel.rowsPerTile = std::max(1, std::min(panel.rowsPerTile,
				rendererInfo.max_texture_height / std::max(1, (int)std::ceil(panel.lineStep))));
		}

		const float colPad = baseColumnPadding_ * font->getMaxHeight() * panel.scale;
		float colsW = 0.0f;
		for (size_t c : panel.visibleColumns) {
			float w = c < table.columns.size() ? measureTextWidthExact(font, table.columns[c], panel.scale) : 0.0f;
			for (const auto& row : table.rows) if (c < row.size()) w = std::max(w, measureTextWidthExact(font, row[c], panel.scale));
			panel.columnWidths.push_back(w); colsW += w;
		}
		if (panel.columnWidths.size() > 1) colsW += colPad * (panel.columnWidths.size() - 1);
		const float startX = std::max(0.0f, (cellW - colsW) * 0.5f);

		panel.header = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
			std::max(1, (int)std::ceil(cellW)), std::max(1, (int)std::ceil(panel.headerHeight)));
		if (panel.header) SDL_SetTextureBlendMode(panel.header, SDL_BLENDMODE_BLEND);
		SDL_Texture* old = SDL_GetRenderTarget(renderer);
		if (panel.header) {
			SDL_SetRenderTarget(renderer, panel.header); SDL_SetRenderDrawColor(renderer, 0,0,0,0); SDL_RenderClear(renderer);
			const float pixelH = panel.scale * font->getMaxHeight(); const auto* mip = font->getMipLevelForSize((int)pixelH);
			if (mip) {
				SDL_SetTextureColorMod(mip->fillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
				if (mip->dynamicFillTexture) SDL_SetTextureColorMod(mip->dynamicFillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
				const float k = mip->height > 0 ? pixelH / mip->height : 1.0f;
				if (!table.id.empty()) renderTextOutlined(renderer, font, mip, mip->fillTexture, mip->outlineTexture, table.id,
					(cellW - measureTextWidthExact(font, table.id, panel.scale)) * 0.5f, 0, panel.scale, k);
				float x = startX, y = reserveTitle ? panel.lineStep : 0.0f;
				for (size_t ci=0; ci<panel.visibleColumns.size(); ++ci) { size_t c=panel.visibleColumns[ci]; std::string s=c<table.columns.size()?table.columns[c]:"";
					renderTextOutlined(renderer,font,mip,mip->fillTexture,mip->outlineTexture,s,x+(panel.columnWidths[ci]-measureTextWidthExact(font,s,panel.scale))*0.5f,y,panel.scale,k); x+=panel.columnWidths[ci]+colPad; }
			}
		}

		for (size_t first=0; first<rowCount; first+=panel.rowsPerTile) {
			const size_t count=std::min<size_t>(panel.rowsPerTile,rowCount-first);
			SDL_Texture* tile=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,std::max(1,(int)std::ceil(cellW)),std::max(1,(int)std::ceil(panel.lineStep*count)));
			if (!tile) {
				continue;
			}
			SDL_SetTextureBlendMode(tile, SDL_BLENDMODE_BLEND);
			SDL_SetRenderTarget(renderer, tile);
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
			SDL_RenderClear(renderer);
			const float pixelH=panel.scale*font->getMaxHeight(); const auto* mip=font->getMipLevelForSize((int)pixelH); if(mip){SDL_SetTextureColorMod(mip->fillTexture,baseViewInfo.textColor.r,baseViewInfo.textColor.g,baseViewInfo.textColor.b);if(mip->dynamicFillTexture)SDL_SetTextureColorMod(mip->dynamicFillTexture,baseViewInfo.textColor.r,baseViewInfo.textColor.g,baseViewInfo.textColor.b);const float k=mip->height>0?pixelH/mip->height:1.0f;
				for(size_t rr=0;rr<count;++rr){
					float x=startX,y=panel.lineStep*rr;
					SDL_Rect rowClip{0,(int)std::floor(y),std::max(1,(int)std::ceil(cellW)),
						std::max(1,(int)std::ceil(panel.lineStep))};
					SDL_RenderSetClipRect(renderer,&rowClip);
					const auto& row=table.rows[first+rr];
					for(size_t ci=0;ci<panel.visibleColumns.size();++ci){size_t c=panel.visibleColumns[ci];std::string s=c<row.size()?row[c]:"";renderTextOutlined(renderer,font,mip,mip->fillTexture,mip->outlineTexture,s,x+(panel.columnWidths[ci]-measureTextWidthExact(font,s,panel.scale))*0.5f,y,panel.scale,k);x+=panel.columnWidths[ci]+colPad;}
				}
				SDL_RenderSetClipRect(renderer,nullptr);
			}
			panel.rowTiles.push_back(tile);
		}
		SDL_SetRenderTarget(renderer, old); pagePanels_.push_back(std::move(panel));
	}
	if (resetPresentation) {
		currentPosition_ = 0.0f;
		pageElapsed_ = 0.0f;
		pageEndPause_ = 0.0f;
		waitStartTime_ = startTime_;
	}
	else {
		float maximumScroll = 0.0f;
		for (const auto& panel : pagePanels_) maximumScroll = std::max(maximumScroll, panel.maxScroll);
		currentPosition_ = std::min(currentPosition_, maximumScroll);
	}
}

void ReloadableHiscores::cancelTableTransition_() {
	tableCrossfading_ = false;
	tableCrossfadeTimer_ = 0.0f;
	wholePageTransition_ = false;
	previousPanelStates_.clear();
	transitioningTableIndices_.clear();
	if (previousTableTexture_) {
		SDL_DestroyTexture(previousTableTexture_);
		previousTableTexture_ = nullptr;
	}
}

void ReloadableHiscores::renderCurrentTable_(SDL_Renderer* renderer, float originX, float originY, Uint8 alpha) const {
	if (!renderer || !headerTexture_) return;

	float effectiveViewWidth = baseViewInfo.MaxWidth;
	if (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
		effectiveViewWidth = baseViewInfo.Width;

	const float x = originX + (effectiveViewWidth - cachedTotalTableWidth_) * 0.5f;
	SDL_SetTextureAlphaMod(headerTexture_, alpha);
	SDL_FRect headerDst{ x, originY, cachedTotalTableWidth_, static_cast<float>(headerTextureHeight_) };
	SDL_RenderCopyF(renderer, headerTexture_, nullptr, &headerDst);

	const float rowsAreaHeight = baseViewInfo.ScaledHeight() - headerTextureHeight_;
	if (!tableRowsTexture_ || tableRowsTextureHeight_ <= 0 || rowsAreaHeight <= 0.0f) return;

	SDL_SetTextureAlphaMod(tableRowsTexture_, alpha);
	const float scrollY = currentPosition_;
	const int visibleH = static_cast<int>(std::min(rowsAreaHeight,
		std::max(0.0f, static_cast<float>(tableRowsTextureHeight_) - scrollY)));
	if (visibleH <= 0) return;

	SDL_Rect src{ 0, static_cast<int>(scrollY), static_cast<int>(cachedTotalTableWidth_), visibleH };
	SDL_FRect dst{ x, originY + headerTextureHeight_, cachedTotalTableWidth_, static_cast<float>(visibleH) };
	SDL_RenderCopyF(renderer, tableRowsTexture_, &src, &dst);
}

void ReloadableHiscores::beginTableTransition_() {
	if (highScoreTable_.tables.size() <= 1) return;
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	if (!renderer || !headerTexture_) return;

	cancelTableTransition_();
	const int w = std::max(1, static_cast<int>(std::ceil(
		(baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth) ? baseViewInfo.Width : baseViewInfo.MaxWidth)));
	const int h = std::max(1, static_cast<int>(std::ceil(baseViewInfo.ScaledHeight())));
	previousTableTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_TARGET, w, h);
	if (!previousTableTexture_) return;
	SDL_SetTextureBlendMode(previousTableTexture_, SDL_BLENDMODE_BLEND);

	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, previousTableTexture_);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);
	renderCurrentTable_(renderer, 0.0f, 0.0f, 255);
	SDL_SetRenderTarget(renderer, oldTarget);

	tableCrossfading_ = true;
	tableCrossfadeTimer_ = 0.0f;
}

void ReloadableHiscores::renderPanel_(SDL_Renderer* renderer, const PagePanel& panel,
	float originX, float originY, Uint8 alpha) const {
	const float x = originX + panel.x;
	if (panel.header) {
		SDL_SetTextureAlphaMod(panel.header, alpha);
		SDL_FRect destination{x, originY, panel.width, panel.headerHeight};
		SDL_RenderCopyF(renderer, panel.header, nullptr, &destination);
	}
	const float viewport = std::max(0.0f, baseViewInfo.ScaledHeight() - panel.headerHeight);
	const float scroll = panel.maxScroll > 0.0f
		? std::min(currentPosition_, panel.maxScroll)
		: 0.0f;
	for (size_t tileIndex = 0; tileIndex < panel.rowTiles.size(); ++tileIndex) {
		SDL_Texture* tile = panel.rowTiles[tileIndex];
		if (!tile) continue;
		int tileWidth = 0;
		int tileHeight = 0;
		SDL_QueryTexture(tile, nullptr, nullptr, &tileWidth, &tileHeight);
		const float top = panel.headerHeight + tileIndex * panel.rowsPerTile * panel.lineStep - scroll;
		const float bottom = top + tileHeight;
		const float clippedTop = std::max(panel.headerHeight, top);
		const float clippedBottom = std::min(panel.headerHeight + viewport, bottom);
		if (clippedBottom <= clippedTop) continue;
		SDL_Rect source{
			0,
			static_cast<int>(std::floor(clippedTop - top)),
			tileWidth,
			std::min(tileHeight - static_cast<int>(std::floor(clippedTop - top)),
				static_cast<int>(std::ceil(clippedBottom - clippedTop)))
		};
		SDL_FRect destination{x, originY + clippedTop, panel.width, static_cast<float>(source.h)};
		SDL_SetTextureAlphaMod(tile, alpha);
		SDL_RenderCopyF(renderer, tile, &source, &destination);
	}
}

void ReloadableHiscores::renderPanels_(SDL_Renderer* renderer, float originX, float originY, Uint8 alpha) const {
	for (const auto& panel : pagePanels_) renderPanel_(renderer, panel, originX, originY, alpha);
}

void ReloadableHiscores::capturePageTransition_(
	bool wholePage,
	const std::vector<size_t>& changedTables) {
	if (pagePanels_.empty()) return;
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	if (!renderer) return;
	cancelTableTransition_();
	previousPanelStates_.reserve(pagePanels_.size());
	for (const auto& panel : pagePanels_) {
		previousPanelStates_.push_back({
			panel.tableIndex,
			panel.visibleColumns,
			panel.columnWidths,
			panel.x,
			panel.width,
			panel.scale,
			panel.lineStep,
			panel.headerHeight
		});
	}
	transitioningTableIndices_.insert(changedTables.begin(), changedTables.end());
	wholePageTransition_ = wholePage;
	const int width = std::max(1, static_cast<int>(std::ceil(
		(baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
			? baseViewInfo.Width
			: baseViewInfo.MaxWidth)));
	const int height = std::max(1, static_cast<int>(std::ceil(baseViewInfo.ScaledHeight())));
	previousTableTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_TARGET, width, height);
	if (!previousTableTexture_) {
		cancelTableTransition_();
		return;
	}
	SDL_SetTextureBlendMode(previousTableTexture_, SDL_BLENDMODE_BLEND);
	SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, previousTableTexture_);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);
	renderPanels_(renderer, 0.0f, 0.0f, 255);
	SDL_SetRenderTarget(renderer, oldTarget);
	tableCrossfading_ = true;
	tableCrossfadeTimer_ = 0.0f;
}

void ReloadableHiscores::beginPageTransition_() {
	capturePageTransition_(true);
}

bool ReloadableHiscores::panelGeometryStable_(const std::vector<size_t>& changedTables) const {
	if (previousPanelStates_.size() != pagePanels_.size()) return false;
	const std::unordered_set<size_t> changed(changedTables.begin(), changedTables.end());
	const auto closeEnough = [](float lhs, float rhs) { return std::abs(lhs - rhs) <= 0.01f; };
	for (const auto& previous : previousPanelStates_) {
		auto current = std::find_if(pagePanels_.begin(), pagePanels_.end(), [&](const PagePanel& panel) {
			return panel.tableIndex == previous.tableIndex;
		});
		if (current == pagePanels_.end()) return false;
		if (!closeEnough(previous.x, current->x) || !closeEnough(previous.width, current->width)) {
			return false;
		}
		if (changed.find(previous.tableIndex) != changed.end()) continue;
		if (!closeEnough(previous.scale, current->scale) ||
			!closeEnough(previous.lineStep, current->lineStep) ||
			!closeEnough(previous.headerHeight, current->headerHeight) ||
			previous.visibleColumns != current->visibleColumns ||
			previous.columnWidths.size() != current->columnWidths.size()) {
			return false;
		}
		for (size_t column = 0; column < previous.columnWidths.size(); ++column) {
			if (!closeEnough(previous.columnWidths[column], current->columnWidths[column])) return false;
		}
	}
	return true;
}

bool ReloadableHiscores::updatePages_(float dt) {
	Item* selected = page.getSelectedItem(displayOffset_);
	const std::string selectedGame = selected ? selected->name : std::string();
	const bool selectionEvent = newItemSelected || (newScrollItemSelected && getMenuScrollReload());
	const bool selectionChanged = selected != lastSelectedItem_ ||
		selectedGame != lastSelectedGame_ || selectionEvent;
	const uint64_t revision = selected
		? LocalHiScores::getInstance().getRevision(localScoreQuery(selected))
		: 0;
	const float currentWidth = (baseViewInfo.Width > 0 && baseViewInfo.Width < baseViewInfo.MaxWidth)
		? baseViewInfo.Width
		: baseViewInfo.MaxWidth;
	const bool geometryChanged =
		std::abs(currentWidth - cachedViewWidth_) > 0.5f ||
		std::abs(baseViewInfo.ScaledHeight() - cachedViewHeight_) > 0.5f ||
		std::abs(baseViewInfo.FontSize - cachedBaseFontSize_) > 0.01f;

	const bool revisionChanged = revision != lastRenderedRevision_;
	if (selectionChanged || revisionChanged || geometryChanged) {
		const bool presentationOnlyRefresh = selected && revisionChanged &&
			!selectionChanged && !geometryChanged;
		const HighScoreView previousView = highScoreTable_;
		const float previousPosition = currentPosition_;
		std::vector<size_t> previousPageTables;
		if (currentPageIndex_ < pagePlan_.size()) {
			previousPageTables = pagePlan_[currentPageIndex_].tableIndices;
		}
		const size_t previousAnchor = previousPageTables.empty()
			? std::numeric_limits<size_t>::max()
			: previousPageTables.front();

		lastSelectedItem_ = selected;
		lastSelectedGame_ = selectedGame;
		if (selectionChanged) {
			currentPageIndex_ = 0;
		}

		HighScoreSnapshot snapshot;
		HighScoreView nextView;
		if (selected) {
			snapshot = LocalHiScores::getInstance().getTable(localScoreQuery(selected));
			nextView = std::move(snapshot.view);
			lastRenderedRevision_ = snapshot.revision;
		}
		else {
			lastRenderedRevision_ = 0;
		}

		const HighScoreViewChange change = compareHighScoreViews(previousView, nextView);
		HighScoreViewChange transitionChange = change;
		if (!change.structureChanged) {
			transitionChange.changedTableIndices.erase(
				std::remove_if(
					transitionChange.changedTableIndices.begin(),
					transitionChange.changedTableIndices.end(),
					[&](size_t tableIndex) {
						return tableIndex < previousView.tables.size() &&
							tableIndex < nextView.tables.size() &&
							highScoreTableChangeIsRowLocal(
								previousView.tables[tableIndex], nextView.tables[tableIndex]);
					}),
				transitionChange.changedTableIndices.end());
		}
		const bool continuingTransition = tableCrossfading_;
		if (presentationOnlyRefresh && change.anyChange() && !continuingTransition) {
			capturePageTransition_(transitionChange.structureChanged, transitionChange.changedTableIndices);
		}
		else if (!presentationOnlyRefresh) {
			cancelTableTransition_();
		}

		highScoreTable_ = std::move(nextView);
		rebuildPagePlan_();
		std::optional<size_t> currentAnchor;
		if (presentationOnlyRefresh && previousAnchor < previousView.tables.size()) {
			currentAnchor = findCorrespondingHighScoreTable(
				previousView.tables[previousAnchor], highScoreTable_, previousAnchor);
		}
		if (currentAnchor) {
			for (size_t pageIndex = 0; pageIndex < pagePlan_.size(); ++pageIndex) {
				const auto& tables = pagePlan_[pageIndex].tableIndices;
				if (std::find(tables.begin(), tables.end(), *currentAnchor) != tables.end()) {
					currentPageIndex_ = pageIndex;
					break;
				}
			}
		}
		buildCurrentPage_(!presentationOnlyRefresh);

		if (presentationOnlyRefresh) {
			const std::vector<size_t> currentPageTables = currentPageIndex_ < pagePlan_.size()
				? pagePlan_[currentPageIndex_].tableIndices
				: std::vector<size_t>{};
			const bool stableScrollPosition =
				std::abs(previousPosition - currentPosition_) <= 0.01f;
			const bool stableDirectGeometry = panelGeometryStable_({});
			const bool rowLocalOnly = change.anyChange() && !transitionChange.anyChange();
			const HighScoreTransitionScope scope = rowLocalOnly
				? (previousPageTables == currentPageTables &&
					stableDirectGeometry && stableScrollPosition
						? HighScoreTransitionScope::None
						: HighScoreTransitionScope::WholePage)
				: chooseHighScoreTransitionScope(
					transitionChange,
					previousPageTables,
					currentPageTables,
					panelGeometryStable_(change.changedTableIndices),
					stableScrollPosition);
			if (scope == HighScoreTransitionScope::None) {
				if (!continuingTransition) cancelTableTransition_();
			}
			else if (scope == HighScoreTransitionScope::WholePage) {
				wholePageTransition_ = true;
				transitioningTableIndices_.clear();
			}
			else {
				if (!wholePageTransition_) {
					transitioningTableIndices_.insert(
						transitionChange.changedTableIndices.begin(),
						transitionChange.changedTableIndices.end());
				}
			}
		}
		newItemSelected = false;
		newScrollItemSelected = false;

		if (pagePlan_.empty()) {
			SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
			FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
			if (renderer && font) {
				renderNoDataMessage(renderer, font);
			}
		}
	}

	const bool componentDone = Component::update(dt);

	// Data stays current while hidden, but presentation time only advances while visible.
	const bool isVisible = baseViewInfo.Alpha > 0.0f;
	if (!isVisible) {
		wasComponentVisible_ = false;
		return componentDone;
	}

	// Re-showing an unsupported notice gives it a fresh hold/fade cycle.
	if (!wasComponentVisible_ && showingNoData_) {
		noDataElapsed_ = 0.0f;
	}
	wasComponentVisible_ = true;

	if (tableCrossfading_) {
		tableCrossfadeTimer_ += dt;
		if (tableCrossfadeTimer_ >= tableCrossfadeDuration_) {
			cancelTableTransition_();
		}
	}

	if (pagePanels_.empty()) {
		if (showingNoData_ && headerTexture_) {
			noDataElapsed_ += dt;
		}
		return componentDone;
	}

	if (waitStartTime_ > 0.0f) {
		waitStartTime_ = std::max(0.0f, waitStartTime_ - dt);
		return componentDone;
	}

	float maxScroll = 0.0f;
	for (const auto& panel : pagePanels_) {
		maxScroll = std::max(maxScroll, panel.maxScroll);
	}

	bool advance = false;
	if (maxScroll > 0.0f) {
		if (currentPosition_ < maxScroll) {
			currentPosition_ = std::min(maxScroll, currentPosition_ + scrollingSpeed_ * dt);
		}
		else {
			pageEndPause_ += dt;
			advance = pageEndPause_ >= startTime_;
		}
	}
	else {
		pageElapsed_ += dt;
		advance = pageElapsed_ >= displayTime_;
	}

	if (advance) {
		if (pagePlan_.size() > 1) {
			beginPageTransition_();
			currentPageIndex_ = (currentPageIndex_ + 1) % pagePlan_.size();
			buildCurrentPage_();
		}
		else if (maxScroll > 0.0f) {
			beginPageTransition_();
			buildCurrentPage_();
		}
		else {
			currentPosition_ = 0.0f;
			pageElapsed_ = 0.0f;
			pageEndPause_ = 0.0f;
			waitStartTime_ = startTime_;
		}
	}

	return componentDone;
}

bool ReloadableHiscores::ensureCompositeTexture_(
	SDL_Renderer* renderer,
	int width,
	int height) {

	if (!renderer || width <= 0 || height <= 0) {
		return false;
	}

	if (compositeTexture_ &&
		compositeTextureWidth_ == width &&
		compositeTextureHeight_ == height)
	{
		return true;
	}

	SDL_Texture* replacement =
		SDL_CreateTexture(
			renderer,
			SDL_PIXELFORMAT_RGBA8888,
			SDL_TEXTUREACCESS_TARGET,
			width,
			height
		);

	if (!replacement) {
		LOG_ERROR(
			"ReloadableHiscores",
			"Failed to create foreground composite texture: " +
			std::string(SDL_GetError())
		);
		return false;
	}

	SDL_SetTextureBlendMode(replacement, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(replacement, SDL_ScaleModeLinear);

	if (compositeTexture_) {
		SDL_DestroyTexture(compositeTexture_);
	}

	compositeTexture_ = replacement;
	compositeTextureWidth_ = width;
	compositeTextureHeight_ = height;
	return true;
}

void ReloadableHiscores::drawPages_() {
	Component::draw();

	if (baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);

	if (!renderer) {
		return;
	}

	const float componentWidth =
		(baseViewInfo.Width > 0.0f &&
			baseViewInfo.Width < baseViewInfo.MaxWidth)
		? baseViewInfo.Width
		: baseViewInfo.MaxWidth;

	const float componentHeight = baseViewInfo.ScaledHeight();

	if (!std::isfinite(componentWidth) ||
		!std::isfinite(componentHeight) ||
		componentWidth <= 0.0f ||
		componentHeight <= 0.0f)
	{
		return;
	}

	const int compositeWidth =
		std::max(1, static_cast<int>(std::ceil(componentWidth)));

	const int compositeHeight =
		std::max(1, static_cast<int>(std::ceil(componentHeight)));

	if (!ensureCompositeTexture_(
		renderer,
		compositeWidth,
		compositeHeight))
	{
		return;
	}

	float noDataOpacity = 1.0f;

	if (pagePanels_.empty()) {
		if (!headerTexture_) {
			return;
		}

		if (showingNoData_ &&
			noDataElapsed_ > kNoDataHoldSeconds)
		{
			noDataOpacity =
				1.0f -
				std::clamp(
					(noDataElapsed_ - kNoDataHoldSeconds) /
						kNoDataFadeSeconds,
					0.0f,
					1.0f
				);
		}

		if (noDataOpacity <= 0.0f) {
			return;
		}
	}

	SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer);

	if (SDL_SetRenderTarget(renderer, compositeTexture_) != 0) {
		LOG_ERROR(
			"ReloadableHiscores",
			"Failed to select foreground composite target: " +
			std::string(SDL_GetError())
		);
		return;
	}

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);

	if (pagePanels_.empty()) {
		renderCurrentTable_(
			renderer,
			0.0f,
			0.0f,
			static_cast<Uint8>(
				std::lround(noDataOpacity * 255.0f)
				)
		);
	}
	else {
		const float fade =
			tableCrossfading_ &&
			tableCrossfadeDuration_ > 0.0f
			? std::clamp(
				tableCrossfadeTimer_ /
					tableCrossfadeDuration_,
				0.0f,
				1.0f
			)
			: 1.0f;

		const Uint8 previousAlpha = static_cast<Uint8>(
			std::lround((1.0f - fade) * 255.0f));
		const Uint8 currentAlpha = static_cast<Uint8>(
			std::lround(fade * 255.0f));

		if (tableCrossfading_ && previousTableTexture_) {
			SDL_SetTextureAlphaMod(
				previousTableTexture_,
				previousAlpha
			);

			if (wholePageTransition_) {
				const SDL_FRect previousDestination{
					0.0f,
					0.0f,
					componentWidth,
					componentHeight
				};
				SDL_RenderCopyF(renderer, previousTableTexture_, nullptr, &previousDestination);
			}
			else {
				for (const auto& previousPanel : previousPanelStates_) {
					if (transitioningTableIndices_.find(previousPanel.tableIndex) ==
						transitioningTableIndices_.end()) {
						continue;
					}
					const int sourceX = std::max(0, static_cast<int>(std::floor(previousPanel.x)));
					const int sourceWidth = std::max(1, std::min(
						compositeWidth - sourceX,
						static_cast<int>(std::ceil(previousPanel.width))));
					if (sourceX >= compositeWidth || sourceWidth <= 0) continue;
					const SDL_Rect source{sourceX, 0, sourceWidth, compositeHeight};
					const SDL_FRect destination{
						previousPanel.x,
						0.0f,
						static_cast<float>(sourceWidth),
						componentHeight
					};
					SDL_RenderCopyF(renderer, previousTableTexture_, &source, &destination);
				}
			}
		}

		if (!tableCrossfading_ || wholePageTransition_) {
			renderPanels_(renderer, 0.0f, 0.0f, currentAlpha);
		}
		else {
			for (const auto& panel : pagePanels_) {
				const Uint8 panelAlpha = transitioningTableIndices_.find(panel.tableIndex) !=
					transitioningTableIndices_.end()
					? currentAlpha
					: 255;
				renderPanel_(renderer, panel, 0.0f, 0.0f, panelAlpha);
			}
		}
	}

	if (SDL_SetRenderTarget(renderer, previousTarget) != 0) {
		LOG_ERROR(
			"ReloadableHiscores",
			"Failed to restore page render target: " +
			std::string(SDL_GetError())
		);
		return;
	}

	SDL_FRect destination{
		baseViewInfo.XRelativeToOrigin(),
		baseViewInfo.YRelativeToOrigin(),
		componentWidth,
		componentHeight
	};

	ViewInfo foregroundView = baseViewInfo;
	foregroundView.ImageWidth =
		static_cast<float>(compositeWidth);
	foregroundView.ImageHeight =
		static_cast<float>(compositeHeight);

	SDL::renderCopyF(
		compositeTexture_,
		baseViewInfo.Alpha,
		nullptr,
		&destination,
		foregroundView,
		page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
		page.getLayoutHeightByMonitor(baseViewInfo.Monitor)
	);
}

void ReloadableHiscores::draw() {
	drawPages_();
}

// Returns final scale and updates column widths and total width
float ReloadableHiscores::computeTableScaleAndWidths(
	FontManager* font,
	const HighScoreTableView& table,
	float& outDrawableHeight,
	float& outRowPadding,
	float& outPaddingBetweenColumns,
	std::vector<float>& outColumnWidths,
	float& outTotalTableWidth,
	float widthConstraint,
	float heightConstraint) {

	const float baseScale = baseViewInfo.FontSize / static_cast<float>(font->getMaxHeight());
	const float baseDrawableHeight = font->getMaxAscent() * baseScale;
	const float baseRowPadding = baseRowPadding_ * baseDrawableHeight;
	const float baseColumnPadding = baseColumnPadding_ * baseDrawableHeight;

	auto measureWidth = [&](float scale, float columnPadding, std::vector<float>* widths) {
		float total = 0.0f;
		if (widths) widths->clear();
		for (size_t visibleIndex = 0; visibleIndex < visibleColumnIndices_.size(); ++visibleIndex) {
			const size_t colIndex = visibleColumnIndices_[visibleIndex];
			float widest = 0.0f;
			if (colIndex < table.columns.size())
				widest = measureTextWidthExact(font, table.columns[colIndex], scale);
			for (const auto& row : table.rows) {
				if (colIndex < row.size())
					widest = std::max(widest, measureTextWidthExact(font, row[colIndex], scale));
			}
			if (widths) widths->push_back(widest);
			total += widest;
			if (visibleIndex + 1 < visibleColumnIndices_.size()) total += columnPadding;
		}
		if (!table.id.empty()) total = std::max(total, measureTextWidthExact(font, table.id, scale));
		return total;
	};

	const float baseWidth = measureWidth(baseScale, baseColumnPadding, nullptr);
	const size_t targetDataRows = std::min<size_t>(10, std::min(table.rows.size(), maxRows_));
	const size_t targetLines = 1 + (table.id.empty() ? 0 : 1) + targetDataRows;
	const float baseHeight = (baseDrawableHeight + baseRowPadding) * static_cast<float>(targetLines);

	const float widthFit = baseWidth > 0.0f ? widthConstraint / baseWidth : 1.0f;
	const float heightFit = baseHeight > 0.0f ? heightConstraint / baseHeight : 1.0f;
	float ratio = std::clamp(std::min({ 1.0f, widthFit, heightFit }), 0.0f, 1.0f);
	ratio = std::floor(ratio * 64.0f) / 64.0f;
	if (ratio <= 0.0f) ratio = 1.0f / 64.0f;

	const float scale = baseScale * ratio;
	const float drawableHeight = font->getMaxAscent() * scale;
	const float rowPadding = baseRowPadding_ * drawableHeight;
	const float columnPadding = baseColumnPadding_ * drawableHeight;
	outTotalTableWidth = measureWidth(scale, columnPadding, &outColumnWidths);
	outDrawableHeight = drawableHeight;
	outRowPadding = rowPadding;
	outPaddingBetweenColumns = columnPadding;
	return scale;
}


void ReloadableHiscores::updateVisibleColumns(const HighScoreTableView& table) {
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
	FontManager* font, const HighScoreTableView& table, float scale, float drawableHeight, float rowPadding, float paddingBetweenColumns, float totalTableWidth) {
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
	SDL_Texture* fillTex = mip->fillTexture;
	SDL_Texture* outlineTex = mip->outlineTexture;

	float y = 0.0f; // This is the baseline y for the current row.

	// Title
	if (!table.id.empty()) {
		float titleWidth = measureTextWidthExact(font, table.id, scale);
		float titleX = (totalTableWidth - titleWidth) / 2.0f;
		renderTextOutlined(renderer, font, mip, fillTex, outlineTex, table.id, titleX, y, scale, mipRelativeScale);
		y += drawableHeight + rowPadding;
	}

	// Headers
	float x = 0.0f;
	for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
		size_t colIndex = visibleColumnIndices_[i];
		const std::string& header = table.columns[colIndex];
		float headerWidth = measureTextWidthExact(font, header, scale);
		float xAligned = x + (cachedColumnWidths_[i] - headerWidth) / 2.0f;
		renderTextOutlined(renderer, font, mip, fillTex, outlineTex, header, xAligned, y, scale, mipRelativeScale);
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
	FontManager* font, const HighScoreTableView& table, float scale, float drawableHeight, float rowPadding, float paddingBetweenColumns, float totalTableWidth) {
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
	SDL_Texture* fillTex = mip->fillTexture;
	SDL_Texture* outlineTex = mip->outlineTexture;

	for (size_t rowIndex = 0; rowIndex < rowsToActuallyRender; ++rowIndex) {
		float y = (drawableHeight + rowPadding) * rowIndex; // This is the baseline y for this row.
		float x = 0.0f;
		for (size_t i = 0; i < visibleColumnIndices_.size(); ++i) {
			size_t colIndex = visibleColumnIndices_[i];
			if (colIndex >= table.rows[rowIndex].size()) {
				x += cachedColumnWidths_[i] + paddingBetweenColumns;
				continue;
			}
			const std::string& cell = table.rows[rowIndex][colIndex];
			float cellWidth = measureTextWidthExact(font, cell, scale);
			float xAligned = x + (cachedColumnWidths_[i] - cellWidth) / 2.0f;
			renderTextOutlined(renderer, font, mip, fillTex, outlineTex, cell, xAligned, y, scale, mipRelativeScale);
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
