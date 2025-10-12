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
#include "SDL_image.h"
#include "../Font.h"

#include <algorithm>
#include <cmath>

ReloadableGlobalHiscores::ReloadableGlobalHiscores(Configuration& /*config*/, std::string textFormat,
	Page& p, int displayOffset,
	FontManager* font,
	float baseColumnPadding, float baseRowPadding)
	: Component(p)
	, fontInst_(font)
	, textFormat_(std::move(textFormat))
	, baseColumnPadding_(baseColumnPadding)
	, baseRowPadding_(baseRowPadding)
	, displayOffset_(displayOffset)
	, needsRedraw_(true)
	, highScoreTable_(nullptr)
	, intermediateTexture_(nullptr) {
	// Grid defaults (can be wired to XML later)
	gridColsHint_ = 0;      // auto near-square
	cellSpacingH_ = 0.02f;  // 2% of width
	cellSpacingV_ = 0.02f;  // 2% of height

	//allocateGraphicsMemory();
}

// Returns {startIndex, count} for the current page.
// Guarantees count >= 0 and loops page index safely.
static inline std::pair<int, int> computeVisibleRange(
	int totalTables, int pageIndex, int pageSize) {
	if (pageSize <= 0) pageSize = 6;                  // safety default
	if (totalTables <= 0) return { 0, 0 };
	const int pageCount = std::max(1, (totalTables + pageSize - 1) / pageSize);
	const int safePage = (pageIndex % pageCount + pageCount) % pageCount; // modulo for safety
	const int start = safePage * pageSize;
	const int count = std::min(pageSize, totalTables - start);
	return { start, count };
}

ReloadableGlobalHiscores::~ReloadableGlobalHiscores() {
	freeGraphicsMemory();
}

bool ReloadableGlobalHiscores::update(float dt) {
	// Clamp weird dt spikes/NaNs
	if (!std::isfinite(dt)) dt = 0.f;
	dt = std::max(0.f, std::min(dt, 0.25f));

	// Drive crossfade timer
	if (fadeActive_) {
		fadeT_ += dt;
		if (fadeT_ >= fadeDurationSec_) {
			fadeT_ = fadeDurationSec_;
			fadeActive_ = false;
		}
	}

	// Debounce timer for selection/scroll reloads
	if (reloadDebounceTimer_ > 0.0f) {
		reloadDebounceTimer_ = std::max(0.0f, reloadDebounceTimer_ - dt);
	}

	const float widthNow = baseViewInfo.Width;
	const float heightNow = baseViewInfo.Height;
	const bool geomChanged = (prevGeomW_ != widthNow || prevGeomH_ != heightNow || prevGeomFont_ != baseViewInfo.FontSize);
	const uint64_t epochNow = HiScores::getInstance().getGlobalEpoch();
	const bool dataChanged = (epochNow != lastEpochSeen_);

	const bool selChange = newItemSelected || (newScrollItemSelected && getMenuScrollReload());

	// This represents “we are about to load a different table set”
	const bool tableContextChanged = (!highScoreTable_) || selChange || dataChanged;

	// If we are switching to a new table context, don't fade this first show
	if (tableContextChanged) {
		hasShownOnce_ = false;   // first paint of the new table should not fade
		fadeActive_ = false;   // nuke any running fade
		fadeStartPending_ = false;
		fadeT_ = 0.0f;
	}

	const bool needHardReload = geomChanged || !highScoreTable_ || dataChanged;
	const bool needSelReload = selChange;

	// Hard triggers: never debounced, no fade on first show
	if (needHardReload) {
		gridPageIndex_ = 0;
		gridTimerSec_ = 0.0f;
		gridBaselineValid_ = false;
        needsRedraw_ = true;

		newItemSelected = false;
		newScrollItemSelected = false;
		prevGeomW_ = widthNow; prevGeomH_ = heightNow; prevGeomFont_ = baseViewInfo.FontSize;
		lastEpochSeen_ = epochNow;

		reloadDebounceTimer_ = reloadDebounceSec_;
		return Component::update(dt);
	}

	// Selection/scroll triggers: debounce; skip fade on first show of new table
	if (needSelReload) {
		if (reloadDebounceTimer_ <= 0.0f) {
			// hasShownOnce_ is already false from the tableContextChanged block above,
			// so we do NOT arm a fade here.
			gridPageIndex_ = 0;
			gridTimerSec_ = 0.0f;
			gridBaselineValid_ = false;
            needsRedraw_ = true;
            reloadDebounceTimer_ = reloadDebounceSec_;
		}
		newItemSelected = false;
		newScrollItemSelected = false;
		return Component::update(dt);
	}

	// Normal page rotation (same table context) — fades are allowed
	if (highScoreTable_ && !highScoreTable_->tables.empty()) {
		const int totalTables = (int)highScoreTable_->tables.size();
		const int pageSize = std::max(1, gridPageSize_);
		const int pageCount = std::max(1, (totalTables + pageSize - 1) / pageSize);

		if (pageCount > 1) {
			gridTimerSec_ += dt;
			if (gridTimerSec_ >= gridRotatePeriodSec_) {
				gridTimerSec_ = 0.0f;
				gridPageIndex_ = (gridPageIndex_ + 1) % pageCount;

				// This is a flip within the same table context — fade is ok
				fadeStartPending_ = true;
				fadeActive_ = true;
				fadeT_ = 0.0f;

                needsRedraw_ = true;
                reloadDebounceTimer_ = reloadDebounceSec_;
			}
		}
		else {
			gridTimerSec_ = 0.0f;
			gridPageIndex_ = 0;
		}
	}

	return Component::update(dt);
}

void ReloadableGlobalHiscores::computeGridBaseline_(
	FontManager* font,
	int totalTables, float compW, float compH,
	float baseScale, float asc) {
	// Decide number of slots in the grid: 6 if we have ?6 tables, else totalTables
	const int slots = (totalTables >= gridPageSize_) ? gridPageSize_ : totalTables;

	// Geometry from fixed 'slots' (NOT from how many are visible on this page)
	int cols = gridColsHint_ > 0 ? gridColsHint_ : (int)std::ceil(std::sqrt((double)slots));
	cols = std::max(1, cols);
	int rows = (slots + cols - 1) / cols;

	const float spacingH = cellSpacingH_ * compW;
	const float spacingV = cellSpacingV_ * compH;
	const float totalW = compW - spacingH * (cols - 1);
	const float totalH = compH - spacingV * (rows - 1);
	const float cellW = totalW / cols;
	const float cellH = totalH / rows;

	// Measure first page (startIdx=0) to produce per-row min scale
	const int firstPageCount = std::min(slots, totalTables);

	std::vector<float> needScale(firstPageCount, 1.0f);
	const float drawableH0 = asc * baseScale;

	for (int t = 0; t < firstPageCount; ++t) {
		const auto& table = highScoreTable_->tables[t];

		const float lineH0 = drawableH0 * (1.0f + baseRowPadding_);
		const float colPad0 = baseColumnPadding_ * drawableH0;

		float width0 = 0.0f;
		for (size_t c = 0; c < table.columns.size(); ++c) {
			float w = (float)font->getWidth(table.columns[c]) * baseScale;
			for (const auto& row : table.rows)
				if (c < row.size()) w = std::max(w, (float)font->getWidth(row[c]) * baseScale);
			width0 += w;
			if (c + 1 < table.columns.size()) width0 += colPad0;
		}
		if (!table.id.empty()) {
			width0 = std::max(width0, (float)font->getWidth(table.id) * baseScale);
		}

		float height0 = lineH0;                    // header
		if (!table.id.empty()) height0 += lineH0;  // title
		height0 += lineH0 * kRowsPerPage;          // rows

		// NOTE: we ignore QR in baseline; if you want to include it, load sizes here.

		const float sW = width0 > 0 ? (cellW / width0) : 1.0f;
		const float sH = height0 > 0 ? (cellH / height0) : 1.0f;
		needScale[t] = std::min({ 1.0f, sW, sH });
	}

	// Per-row minimum over the first-page items
	std::vector<float> rowMin(rows, 1.0f);
	for (int r = 0; r < rows; ++r) {
		float s = 1.0f;
		for (int c = 0; c < cols; ++c) {
			int i = r * cols + c; if (i >= firstPageCount) break;
			s = std::min(s, needScale[i]);
		}
		rowMin[r] = s;
	}

	// Save as baseline
	gridBaselineSlots_ = slots;
	gridBaselineCols_ = cols;
	gridBaselineRows_ = rows;
	gridBaselineCellW_ = cellW;
	gridBaselineCellH_ = cellH;
	gridBaselineRowMin_ = std::move(rowMin);
	gridBaselineValid_ = true;
}


void ReloadableGlobalHiscores::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	reloadTexture();
}

void ReloadableGlobalHiscores::freeGraphicsMemory() {
	Component::freeGraphicsMemory();

	if (intermediateTexture_) {
		SDL_DestroyTexture(intermediateTexture_);
		intermediateTexture_ = nullptr;
	}
	if (prevCompositeTexture_) { SDL_DestroyTexture(prevCompositeTexture_); prevCompositeTexture_ = nullptr; }

	destroyAllQr_();
}


void ReloadableGlobalHiscores::deInitializeFonts() {
	if (fontInst_) fontInst_->deInitialize();
}

void ReloadableGlobalHiscores::initializeFonts() {
	if (fontInst_) fontInst_->initialize();
}

void ReloadableGlobalHiscores::reloadTexture() {
    // --- renderer for mipmapped outlined glyphs (unchanged except kerning guard) ----
    auto renderTextOutlined = [&](SDL_Renderer* r, FontManager* f,
        const std::string& s, float x, float y, float finalScale) {
            if (s.empty()) return;

            const float targetH = finalScale * f->getMaxHeight();
            const FontManager::MipLevel* mip = f->getMipLevelForSize((int)targetH);
            if (!mip || !mip->fillTexture) return;

            const float k = (mip->height > 0) ? (targetH / mip->height) : 1.0f;
            SDL_Texture* fillTex = mip->fillTexture;
            SDL_Texture* outlineTex = mip->outlineTexture;

            const float ySnap = std::round(y);

            // Outline pass
            if (outlineTex) {
                float penX = std::round(x);
                Uint16 prev = 0;
                for (unsigned char uc : s) {
                    Uint16 ch = (Uint16)uc;
                    if (prev) penX += f->getKerning(prev, ch) * finalScale;

                    auto it = mip->glyphs.find(ch);
                    if (it != mip->glyphs.end()) {
                        const auto& g = it->second;
                        const SDL_Rect& src = g.rect;
                        SDL_FRect dst = {
                            penX - g.fillX * k,
                            ySnap - g.fillY * k,
                            src.w * k, src.h * k
                        };
                        SDL_RenderCopyF(r, outlineTex, &src, &dst);
                        penX += g.advance * k;
                    }
                    prev = ch;
                }
            }

            // Fill pass
            {
                float penX = std::round(x);
                Uint16 prev = 0;
                for (unsigned char uc : s) {
                    Uint16 ch = (Uint16)uc;
                    if (prev) penX += f->getKerning(prev, ch) * finalScale;

                    auto it = mip->glyphs.find(ch);
                    if (it != mip->glyphs.end()) {
                        const auto& g = it->second;
                        SDL_Rect srcFill{ g.rect.x + g.fillX, g.rect.y + g.fillY, g.fillW, g.fillH };
                        SDL_FRect dst{ penX, ySnap, g.fillW * k, g.fillH * k };
                        SDL_RenderCopyF(r, fillTex, &srcFill, &dst);
                        penX += g.advance * k;
                    }
                    prev = ch;
                }
            }
        };

    // --- exact width measure (mip + kerning + outline overhang) -----------------------
    auto measureTextWidthExact = [&](FontManager* f, const std::string& s, float scale) -> float {
        if (!f || s.empty()) return 0.0f;
        const float targetH = scale * f->getMaxHeight();
        const FontManager::MipLevel* mip = f->getMipLevelForSize((int)targetH);
        if (!mip || !mip->fillTexture) return (float)f->getWidth(s) * scale;

        const float k = (mip->height > 0) ? (targetH / mip->height) : 1.0f;
        const bool hasOutline = (mip->outlineTexture != nullptr);

        float penX = 0.0f, minX = 0.0f, maxX = 0.0f;
        bool first = true;
        Uint16 prev = 0;
        for (unsigned char uc : s) {
            Uint16 ch = (Uint16)uc;
            if (prev) penX += f->getKerning(prev, ch) * scale;

            auto it = mip->glyphs.find(ch);
            if (it != mip->glyphs.end()) {
                const auto& g = it->second;

                float left = penX;
                float right = penX + g.fillW * k;

                if (hasOutline) {
                    const float oL = penX - g.fillX * k;
                    const float oR = oL + g.rect.w * k;
                    left = std::min(left, oL);
                    right = std::max(right, oR);
                }

                if (first) { minX = left; maxX = right; first = false; }
                else { minX = std::min(minX, left); maxX = std::max(maxX, right); }

                penX += g.advance * k;
            }
            prev = ch;
        }
        return std::max(0.0f, maxX - minX);
        };

    // --- helpers -----------------------------------------------------------------------
    auto isPlaceholderCell = [](const std::string& s) -> bool {
        size_t l = s.find_first_not_of(" \t");
        if (l == std::string::npos) return false;
        size_t r = s.find_last_not_of(" \t");
        const std::string v = s.substr(l, r - l + 1);
        if (v == "-" || v == "$-" || v == "--:--:---") return true;
        for (unsigned char uc : v) {
            char c = (char)uc;
            if (c == '-' || c == ':' || c == '.' || c == '$' || c == ' ') continue;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return false;
            return false;
        }
        return true;
        };

    enum class ColAlign { Left, Center, Right };
    auto colAlignFor = [](size_t idx, size_t nCols) -> ColAlign {
        if (nCols >= 4) {
            if (idx == 0) return ColAlign::Left;
            if (idx == 1) return ColAlign::Left;
            if (idx == 2) return ColAlign::Right;
            if (idx == 3) return ColAlign::Right;
        }
        return ColAlign::Center;
        };
    auto alignX = [](float x, float colW, float textW, ColAlign a) -> float {
        switch (a) {
            case ColAlign::Left:   return x + 1.0f; // 1px guard
            case ColAlign::Center: return x + (colW - textW) * 0.5f;
            case ColAlign::Right:  return x + (colW - textW);
        }
        return x;
        };
    auto clampf = [](float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); };
    auto quantize_down = [](float s) { const float q = 64.f; return std::max(0.f, std::floor(s * q) / q); };

    // --- setup / early-outs ------------------------------------------------------------
    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    destroyAllQr_();

    Item* selectedItem = page.getSelectedItem(displayOffset_);
    if (!selectedItem || !renderer) {
        highScoreTable_ = nullptr;
        if (intermediateTexture_) {
            SDL_Texture* old = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, intermediateTexture_);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_SetRenderTarget(renderer, old);
        }
        hasShownOnce_ = true;
        needsRedraw_ = false;
        return;
    }

    highScoreTable_ = HiScores::getInstance().getGlobalHiScoreTable(selectedItem);
    if (!highScoreTable_ || highScoreTable_->tables.empty()) {
        if (intermediateTexture_) {
            SDL_Texture* old = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, intermediateTexture_);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_SetRenderTarget(renderer, old);
        }
        hasShownOnce_ = true;
        needsRedraw_ = false;
        return;
    }

    const int totalTables = (int)highScoreTable_->tables.size();
    const float compW = baseViewInfo.Width;
    const float compH = baseViewInfo.Height;

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font) { needsRedraw_ = true; return; }

    // --- base metrics --------------------------------------------------------
    const float baseScale = baseViewInfo.FontSize / (float)font->getMaxHeight();
    const float asc = (float)font->getMaxAscent();

    const float drawableH0 = asc * baseScale;
    const float lineH0 = drawableH0 * (1.0f + baseRowPadding_);
    const float colPad0 = baseColumnPadding_ * drawableH0;

    constexpr float kPanelGuardPx = 1.0f;  // prevents rounding/overhang shaves

    // Create/resize composite RT
    const int compositeW = (int)std::lround(compW);
    const int compositeH = (int)std::lround(compH);
    if (!intermediateTexture_ || compW_ != compositeW || compH_ != compositeH) {
        if (intermediateTexture_) SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_TARGET, compositeW, compositeH);
        if (!intermediateTexture_) { needsRedraw_ = true; return; }
        SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
        compW_ = compositeW; compH_ = compositeH;
        if (prevCompositeTexture_) { SDL_DestroyTexture(prevCompositeTexture_); prevCompositeTexture_ = nullptr; }
    }

    if (!gridBaselineValid_) {
        computeGridBaseline_(font, totalTables, compW, compH, baseScale, asc);
    }
    const int   cols = gridBaselineCols_;
    const int   rows = gridBaselineRows_;
    const float cellW = gridBaselineCellW_;
    const float cellH = gridBaselineCellH_;
    const float spacingH = cellSpacingH_ * compW;
    const float spacingV = cellSpacingV_ * compH;

    const auto [startIdx, Nvisible] =
        computeVisibleRange(totalTables, gridPageIndex_, std::max(1, gridPageSize_));
    if (Nvisible <= 0) { needsRedraw_ = true; return; }

    // --- Load QRs for visible set -------------------------------------------------------
    std::vector<std::string> gameIds;
    if (!selectedItem->iscoredId.empty())
        Utils::listToVector(selectedItem->iscoredId, gameIds, ',');

    qrByTable_.assign(Nvisible, {});
    for (int t = 0; t < Nvisible; ++t) {
        const int gi = startIdx + t;
        if (gi < (int)gameIds.size() && !gameIds[gi].empty()) {
            std::string path = Configuration::absolutePath + "/iScored/qr/" + gameIds[gi] + ".png";
            if (SDL_Texture* tex = IMG_LoadTexture(renderer, path.c_str())) {
                int w = 0, h = 0; SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                qrByTable_[t] = { tex, w, h, true };
            }
        }
    }

    // --- QR reservation (size + placement) ----------------------------------------------
    struct Margins { float L = 0, R = 0, T = 0, B = 0; };
    std::vector<Margins> qrReserve(Nvisible);
    const bool reserveVerticalForCorners = false;  // set true if corners should consume height
    const bool reserveHorizontalForSides = true;

    for (int t = 0; t < Nvisible; ++t) {
        if (t >= (int)qrByTable_.size() || !qrByTable_[t].ok) continue;
        const auto& q = qrByTable_[t];
        auto& m = qrReserve[t];

        switch (qrPlacement_) {
            case QrPlacement::TopCentered:    m.T = (float)qrMarginPx_ + (float)q.h; break;
            case QrPlacement::BottomCenter:   m.B = (float)qrMarginPx_ + (float)q.h; break;
            case QrPlacement::LeftMiddle:     if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)q.w; break;
            case QrPlacement::RightMiddle:    if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)q.w; break;
            case QrPlacement::TopLeft:        if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)q.w;
                if (reserveVerticalForCorners)  m.T = (float)qrMarginPx_ + (float)q.h; break;
            case QrPlacement::TopRight:       if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)q.w;
                if (reserveVerticalForCorners)  m.T = (float)qrMarginPx_ + (float)q.h; break;
            case QrPlacement::BottomLeft:     if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)q.w;
                if (reserveVerticalForCorners)  m.B = (float)qrMarginPx_ + (float)q.h; break;
            case QrPlacement::BottomRight:    if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)q.w;
                if (reserveVerticalForCorners)  m.B = (float)qrMarginPx_ + (float)q.h; break;
            default: break;
        }
    }

    std::vector<float> qrExtraW(Nvisible, 0.0f), qrExtraH(Nvisible, 0.0f);
    for (int t = 0; t < Nvisible; ++t) {
        qrExtraW[t] = qrReserve[t].L + qrReserve[t].R;
        qrExtraH[t] = qrReserve[t].T + qrReserve[t].B;
    }

    // --- Shared column layout (measure exact widths at baseScale) ------------------------
    size_t maxCols = 0;
    for (int t = 0; t < Nvisible; ++t)
        maxCols = std::max(maxCols, highScoreTable_->tables[startIdx + t].columns.size());
    if (maxCols == 0) { needsRedraw_ = true; return; }

    std::vector<float> maxColW0(maxCols, 0.0f);
    float maxTitleW0 = 0.0f;
    std::vector<float> height0(Nvisible, lineH0 * (1 + kRowsPerPage));

    for (int t = 0; t < Nvisible; ++t) {
        const auto& table = highScoreTable_->tables[startIdx + t];

        for (size_t c = 0; c < maxCols; ++c) {
            float w = 0.0f;
            if (c < table.columns.size()) {
                w = measureTextWidthExact(font, table.columns[c], baseScale);
                for (const auto& row : table.rows)
                    if (c < row.size())
                        w = std::max(w, measureTextWidthExact(font, row[c], baseScale));
            }
            maxColW0[c] = std::max(maxColW0[c], w);
        }

        if (!table.id.empty())
            maxTitleW0 = std::max(maxTitleW0, measureTextWidthExact(font, table.id, baseScale));

        float h = lineH0;                    // header
        if (!table.id.empty()) h += lineH0;  // title
        h += lineH0 * kRowsPerPage;          // rows
        height0[t] = h;
    }

    // --- Build exact shared width @ baseScale (columns + pads + guard) -------------------
    float sumCols0 = 0.0f; for (float w : maxColW0) sumCols0 += w;

    float sharedPad0 = colPad0;
    float sharedW0_exact = sumCols0 + (float)(std::max<size_t>(1, maxCols) - 1) * sharedPad0;

    if (sharedW0_exact < maxTitleW0) {
        const int gaps = (int)std::max<size_t>(1, maxCols - 1);
        const float grow0 = maxTitleW0 - sharedW0_exact;
        sharedPad0 += grow0 / (float)gaps;
        sharedW0_exact = maxTitleW0;
    }
    sharedW0_exact += 2.0f * kPanelGuardPx;   // keep consistent at draw time

    // --- Fit: compute per-slot scale needs using the exact width/height ------------------
    std::vector<float> needScale(Nvisible, 1.0f);
    constexpr float fitEps = 0.5f;
    for (int t = 0; t < Nvisible; ++t) {
        const float availW = std::max(0.0f, cellW - qrExtraW[t] - fitEps);
        const float availH = std::max(0.0f, cellH - qrExtraH[t] - fitEps);

        float sW = sharedW0_exact > 0 ? (availW / sharedW0_exact) : 1.0f;
        float sH = height0[t] > 0 ? (availH / height0[t]) : 1.0f;
        needScale[t] = std::min(std::min(1.0f, std::max(0.0f, sW)),
            std::min(1.0f, std::max(0.0f, sH)));
    }

    // --- Row-wise min, then **quantize DOWN** so draw never exceeds the fit --------------
    std::vector<float> rowScale(rows, 1.0f);
    for (int r = 0; r < rows; ++r) {
        float s = 1.0f;
        for (int c = 0; c < cols; ++c) {
            int i = r * cols + c; if (i >= Nvisible) break;
            s = std::min(s, needScale[i]);
        }
        rowScale[r] = quantize_down(s);
    }

    // --- Fade snapshot if needed ---------------------------------------------------------
    if (fadeStartPending_) {
        if (!prevCompositeTexture_) {
            prevCompositeTexture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_TARGET, compositeW, compositeH);
            SDL_SetTextureBlendMode(prevCompositeTexture_, SDL_BLENDMODE_BLEND);
        }
        SDL_Texture* oldRT = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, prevCompositeTexture_);
        if (intermediateTexture_) SDL_RenderCopy(renderer, intermediateTexture_, nullptr, nullptr);
        SDL_SetRenderTarget(renderer, oldRT);
        fadeStartPending_ = false;
    }

    // --- Paint to composite --------------------------------------------------------------
    SDL_Texture* old = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, intermediateTexture_);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    for (int t = 0; t < Nvisible; ++t) {
        const int gi = startIdx + t;
        const auto& table = highScoreTable_->tables[gi];

        const int slotCol = (t % cols);
        const int slotRow = (t / cols);

        const float xCell = slotCol * (cellW + spacingH);
        const float yCell = slotRow * (cellH + spacingV);

        const float finalScale = baseScale * rowScale[std::min(slotRow, rows - 1)];
        const float ratio = (baseScale > 0.f) ? (finalScale / baseScale) : 1.0f;

        const float drawableH = asc * finalScale;
        const float lineH = drawableH * (1.0f + baseRowPadding_);
        const float colPad = sharedPad0 * ratio;

        // Per-column widths at finalScale (exact)
        std::vector<float> colW(maxCols, 0.0f);
        float totalWCols = 0.0f;
        for (size_t c = 0; c < maxCols; ++c) {
            colW[c] = maxColW0[c] * ratio;
            totalWCols += colW[c];
            if (c + 1 < maxCols) totalWCols += colPad;
        }
        totalWCols += 2.0f * kPanelGuardPx;   // must match the fit side

        // Heights
        const float titleH = table.id.empty() ? 0.0f : lineH;
        const float headerH = lineH;
        const float dataH = lineH * kRowsPerPage;
        const float panelH = std::ceil(titleH + headerH + dataH);
        const float panelW = std::ceil(totalWCols);

        // QR margins (same as used in fit)
        const int extraL = (int)std::lround(qrReserve[t].L);
        const int extraR = (int)std::lround(qrReserve[t].R);
        const int extraT = (int)std::lround(qrReserve[t].T);
        const int extraB = (int)std::lround(qrReserve[t].B);

        // Anchor inside cell
        float anchorW = panelW + (float)(extraL + extraR);
        float anchorH = panelH + (float)(extraT + extraB);

        float anchorX = xCell + (cellW - anchorW) * 0.5f;
        float anchorY = yCell;
        anchorX = std::round(clampf(anchorX, xCell, xCell + cellW - anchorW));
        anchorY = std::round(clampf(anchorY, yCell, yCell + cellH - anchorH));

        // Text origin
        float drawX0 = anchorX + (float)extraL + kPanelGuardPx;
        float y = anchorY + (float)extraT;

        // ---- Title ----------------------------------------------------------
        if (!table.id.empty()) {
            float w = measureTextWidthExact(font, table.id, finalScale);
            float x = std::round((totalWCols - w) * 0.5f);
            renderTextOutlined(renderer, font, table.id, drawX0 + x, y, finalScale);
            y += lineH;
        }

        // ---- Headers --------------------------------------------------------
        {
            float x = 0.0f;
            for (size_t c = 0; c < maxCols; ++c) {
                if (c < table.columns.size()) {
                    const std::string& header = table.columns[c];
                    float hw = measureTextWidthExact(font, header, finalScale);
                    float xAligned = std::round(drawX0 + x + (colW[c] - hw) * 0.5f);
                    renderTextOutlined(renderer, font, header, xAligned, y, finalScale);
                }
                x += colW[c];
                if (c + 1 < maxCols) x += colPad;
            }
            y += lineH;
        }

        // ---- Rows -----------------------------------------------------------
        for (int r = 0; r < kRowsPerPage; ++r) {
            float x = 0.0f;
            const auto* rowV = (r < (int)table.rows.size()) ? &table.rows[r] : nullptr;
            for (size_t c = 0; c < maxCols; ++c) {
                std::string cell; if (rowV && c < rowV->size()) cell = (*rowV)[c];
                const float tw = measureTextWidthExact(font, cell, finalScale);
                const bool ph = isPlaceholderCell(cell);
                const ColAlign a = ph ? ColAlign::Center : colAlignFor(c, maxCols);
                const float xAligned = alignX(drawX0 + x, colW[c], tw, a);
                if (!cell.empty())
                    renderTextOutlined(renderer, font, cell, std::round(xAligned), y, finalScale);
                x += colW[c];
                if (c + 1 < maxCols) x += colPad;
            }
            y += lineH;
        }

#ifndef NDEBUG
        // Panel outline (debug) — where text is actually budgeted to draw
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        SDL_Rect outline = {
            (int)std::lround(drawX0 - kPanelGuardPx),
            (int)std::lround(anchorY + (float)extraT),
            (int)std::lround(panelW),
            (int)std::lround(panelH)
        };
        SDL_RenderDrawRect(renderer, &outline);
#endif

        // ---- QR -------------------------------------------------------------
        if (t < (int)qrByTable_.size() && qrByTable_[t].ok) {
            const QrEntry& q = qrByTable_[t];
            SDL_FRect panelRect = { drawX0 - kPanelGuardPx, anchorY + (float)extraT, panelW, panelH };
            float qrX = 0.f, qrY = 0.f;
            switch (qrPlacement_) {
                case QrPlacement::TopCentered:   qrX = panelRect.x + (panelRect.w - q.w) * 0.5f; qrY = panelRect.y - qrMarginPx_ - q.h; break;
                case QrPlacement::BottomCenter:  qrX = panelRect.x + (panelRect.w - q.w) * 0.5f; qrY = panelRect.y + panelRect.h + qrMarginPx_; break;
                case QrPlacement::TopRight:      qrX = panelRect.x + panelRect.w + qrMarginPx_;   qrY = panelRect.y + qrMarginPx_; break;
                case QrPlacement::TopLeft:       qrX = panelRect.x - qrMarginPx_ - q.w;           qrY = panelRect.y + qrMarginPx_; break;
                case QrPlacement::BottomRight:   qrX = panelRect.x + panelRect.w + qrMarginPx_;   qrY = panelRect.y + panelRect.h - q.h - qrMarginPx_; break;
                case QrPlacement::BottomLeft:    qrX = panelRect.x - qrMarginPx_ - q.w;           qrY = panelRect.y + panelRect.h - q.h - qrMarginPx_; break;
                case QrPlacement::RightMiddle:   qrX = panelRect.x + panelRect.w + qrMarginPx_;   qrY = panelRect.y + (panelRect.h - q.h) * 0.5f; break;
                case QrPlacement::LeftMiddle:    qrX = panelRect.x - qrMarginPx_ - q.w;           qrY = panelRect.y + (panelRect.h - q.h) * 0.5f; break;
                default:                          qrX = panelRect.x + (panelRect.w - q.w) * 0.5f; qrY = panelRect.y + panelRect.h + qrMarginPx_; break;
            }
            if (font) {
                const SDL_Color c = font->getColor();
                Uint8 mx = std::max({ c.r,c.g,c.b });
                SDL_SetTextureColorMod(q.tex, mx, mx, mx);
                SDL_SetTextureAlphaMod(q.tex, c.a);
                SDL_SetTextureBlendMode(q.tex, SDL_BLENDMODE_BLEND);
            }
            SDL_FRect qrDst = { qrX, qrY, (float)q.w, (float)q.h };
            SDL_RenderCopyF(renderer, q.tex, nullptr, &qrDst);
        }
    }

#ifndef NDEBUG
    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
    SDL_Rect outlineRect = { 0, 0, compositeW - 1, compositeH - 1 };
    SDL_RenderDrawRect(renderer, &outlineRect);
#endif

    SDL_SetRenderTarget(renderer, old);
    hasShownOnce_ = true;
    needsRedraw_ = false;
}

void ReloadableGlobalHiscores::draw() {
    Component::draw();

    if (baseViewInfo.Alpha <= 0.0f) return;

    // Make sure the composite is current (this may size RTs, snapshot for fade, and paint)
    if (needsRedraw_) {
        reloadTexture();               // sets needsRedraw_ = false (or clears to blank if no data)
    }

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (!renderer) return;

    // If we don't have a composite, nothing to present
    if (!intermediateTexture_) return;

    SDL_FRect rect = {
        baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
        baseViewInfo.ScaledWidth(),       baseViewInfo.ScaledHeight()
    };

    if (fadeActive_ && prevCompositeTexture_) {
        float t = (fadeDurationSec_ > 0.f) ? (fadeT_ / fadeDurationSec_) : 1.f;
        if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
        const float A = baseViewInfo.Alpha;

        // Old page fades out
        SDL::renderCopyF(
            prevCompositeTexture_, A * (1.0f - t), nullptr, &rect, baseViewInfo,
            page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
            page.getLayoutHeightByMonitor(baseViewInfo.Monitor));

        // New page fades in
        SDL::renderCopyF(
            intermediateTexture_, A * t, nullptr, &rect, baseViewInfo,
            page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
            page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
    }
    else {
        // Normal draw
        SDL::renderCopyF(
            intermediateTexture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
            page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
            page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
    }

    // If the fade has completed (update() turned fadeActive_ off), release the snapshot here.
    if (!fadeActive_ && prevCompositeTexture_) {
        SDL_DestroyTexture(prevCompositeTexture_);
        prevCompositeTexture_ = nullptr;
    }
}


void ReloadableGlobalHiscores::destroyAllQr_() {
	for (auto& q : qrByTable_) {
		if (q.tex) SDL_DestroyTexture(q.tex);
		q = {};
	}
	qrByTable_.clear();
}