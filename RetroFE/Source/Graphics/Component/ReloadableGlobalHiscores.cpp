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

#include <algorithm>
#include <cmath>
#include <limits>


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
    , lastSelectedItem_(nullptr)
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

    const float widthNow = baseViewInfo.Width;
    const float heightNow = baseViewInfo.Height;
    static float prevW = -1.f, prevH = -1.f, prevFont = -1.f;

    const bool geomChanged = (prevW != widthNow || prevH != heightNow || prevFont != baseViewInfo.FontSize);
    const uint64_t epochNow = HiScores::getInstance().getGlobalEpoch();
    const bool dataChanged = (epochNow != lastEpochSeen_);

    // Reset paging and rebuild on any geometry/selection/data change
    if (geomChanged || !highScoreTable_ || newItemSelected || (newScrollItemSelected && getMenuScrollReload()) || dataChanged) {
        gridPageIndex_ = 0;
        gridTimerSec_ = 0.0f;
        gridBaselineValid_ = false;   // <— invalidate baseline so it re-computes
        reloadTexture(true);
        newItemSelected = false;
        newScrollItemSelected = false;
        prevW = widthNow; prevH = heightNow; prevFont = baseViewInfo.FontSize;
        lastEpochSeen_ = epochNow;
    }
    else {
        // Drive rotation by dt only if we actually have multiple pages
        if (highScoreTable_ && !highScoreTable_->tables.empty()) {
            const int totalTables = (int)highScoreTable_->tables.size();
            const int pageSize = std::max(1, gridPageSize_);
            const int pageCount = std::max(1, (totalTables + pageSize - 1) / pageSize);
            if (pageCount > 1) {
                gridTimerSec_ += dt;
                if (gridTimerSec_ >= gridRotatePeriodSec_) {
                    gridTimerSec_ = 0.0f;
                    gridPageIndex_ = (gridPageIndex_ + 1) % pageCount;
                    reloadTexture(true); // rebuild just the new slice
                }
            }
            else {
                gridTimerSec_ = 0.0f;
                gridPageIndex_ = 0;
            }
        }
    }

    return Component::update(dt);
}

void ReloadableGlobalHiscores::computeGridBaseline_(
    SDL_Renderer* renderer, FontManager* font,
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
    reloadTexture(true);
}

void ReloadableGlobalHiscores::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    for (auto& p : tablePanels_) {
        if (p.tex) { SDL_DestroyTexture(p.tex); p.tex = nullptr; }
    }
    tablePanels_.clear();

    if (intermediateTexture_) {
        SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = nullptr;
    }

    destroyAllQr_(); // <— add this
}


void ReloadableGlobalHiscores::deInitializeFonts() {
    if (fontInst_) fontInst_->deInitialize();
}

void ReloadableGlobalHiscores::initializeFonts() {
    if (fontInst_) fontInst_->initialize();
}

void ReloadableGlobalHiscores::reloadTexture(bool /*reset*/) {
    auto renderTextWithKerning = [&](SDL_Renderer* r, FontManager* font,
        const std::string& s, float x, float y, float scale) {
            float cx = std::round(x);
            const float ySnap = std::round(y);
            Uint16 prev = 0;
            for (unsigned char uc : s) {
                Uint16 ch = (Uint16)uc;
                cx += font->getKerning(prev, ch) * scale;
                FontManager::GlyphInfo g;
                if (font->getRect(ch, g)) {
                    SDL_Rect  src = g.rect;
                    SDL_FRect dst = { cx, ySnap, g.rect.w * scale, g.rect.h * scale };
                    SDL_RenderCopyF(r, font->getTexture(), &src, &dst);
                    cx += g.advance * scale;
                }
                prev = ch;
            }
        };

    auto renderTextOutlined = [&](SDL_Renderer* r, FontManager* f,
        const std::string& s, float x, float y, float scale) {
            if (SDL_Texture* outline = f->getOutlineTexture()) {
                float cx = std::round(x);
                const float ySnap = std::round(y);
                Uint16 prev = 0;
                for (unsigned char uc : s) {
                    Uint16 ch = (Uint16)uc;
                    cx += f->getKerning(prev, ch) * scale;
                    FontManager::GlyphInfo g;
                    if (f->getRect(ch, g)) {
                        SDL_Rect  src = g.rect;
                        SDL_FRect dst = { cx, ySnap, g.rect.w * scale, g.rect.h * scale };
                        SDL_RenderCopyF(r, outline, &src, &dst);
                        cx += g.advance * scale;
                    }
                    prev = ch;
                }
            }
            renderTextWithKerning(r, f, s, x, y, scale);
        };

    // Center-align placeholders like "-", "--:--:---", "$-"
    auto isPlaceholderCell = [](const std::string& s) -> bool {
        // trim ASCII spaces/tabs
        size_t l = s.find_first_not_of(" \t");
        if (l == std::string::npos) return false;
        size_t r = s.find_last_not_of(" \t");
        const std::string v = s.substr(l, r - l + 1);

        // Fast-path for exact forms we know HiScores emits
        if (v == "-" || v == "$-" || v == "--:--:---") return true;

        // Generic: treat as placeholder if it contains ONLY these symbols
        // and at least one dash; avoids catching real values.
        bool hasDash = false;
        for (unsigned char uc : v) {
            char c = (char)uc;
            if (c == '-') { hasDash = true; continue; }
            if (c == ':' || c == '.' || c == '$' || c == ' ') continue;
            // any letter/digit/other -> not a placeholder
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                return false;
            // any other symbol -> bail out (conservative)
            return false;
        }
        return hasDash;
        };

    enum class ColAlign { Left, Center, Right };
    auto colAlignFor = [](size_t idx, size_t nCols) -> ColAlign {
        if (nCols >= 4) {
            if (idx == 0) return ColAlign::Left;   // Rank
            if (idx == 1) return ColAlign::Left;   // Name
            if (idx == 2) return ColAlign::Right;  // Score/Time
            if (idx == 3) return ColAlign::Right;  // Date
        }
        return ColAlign::Center;
        };
    auto alignX = [](float x, float colW, float textW, ColAlign a) -> float {
        switch (a) {
            case ColAlign::Left:   return x;
            case ColAlign::Center: return x + (colW - textW) * 0.5f;
            case ColAlign::Right:  return x + (colW - textW);
        }
        return x;
        };

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);

    // clear old textures
    for (auto& p : tablePanels_) if (p.tex) { SDL_DestroyTexture(p.tex); p.tex = nullptr; }
    tablePanels_.clear();
    for (auto& q : qrByTable_) if (q.tex) SDL_DestroyTexture(q.tex);
    qrByTable_.clear();

    // fetch selected & table
    Item* selectedItem = page.getSelectedItem(displayOffset_);
    if (!selectedItem || !renderer) { highScoreTable_ = nullptr; needsRedraw_ = true; return; }
    highScoreTable_ = HiScores::getInstance().getGlobalHiScoreTable(selectedItem);
    if (!highScoreTable_ || highScoreTable_->tables.empty()) { needsRedraw_ = true; return; }

    const int totalTables = (int)highScoreTable_->tables.size();

    // Component size, font info
    const float compW = baseViewInfo.Width;
    const float compH = baseViewInfo.Height;

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font) { needsRedraw_ = true; return; }
    const float baseScale = baseViewInfo.FontSize / (float)font->getHeight();
    const float asc = (float)font->getAscent();

    // Baseline: compute once when invalidated
    if (!gridBaselineValid_) {
        computeGridBaseline_(renderer, font, totalTables, compW, compH, baseScale, asc);
    }

    // Visible slice indices (but grid stays fixed by baseline)
    const auto [startIdx, Nvisible] = computeVisibleRange(totalTables, gridPageIndex_, std::max(1, gridPageSize_));
    if (Nvisible <= 0) { needsRedraw_ = true; return; }

    // Load QRs only for the visible set
    std::vector<std::string> gameIds;
    if (!selectedItem->iscoredId.empty()) Utils::listToVector(selectedItem->iscoredId, gameIds, ',');
    qrByTable_.assign(Nvisible, {});
    for (int t = 0; t < Nvisible; ++t) {
        const int globalIdx = startIdx + t;
        if (globalIdx < (int)gameIds.size() && !gameIds[globalIdx].empty()) {
            std::string path = Configuration::absolutePath + "/iScored/qr/" + gameIds[globalIdx] + ".png";
            if (SDL_Texture* tex = IMG_LoadTexture(renderer, path.c_str())) {
                int w = 0, h = 0; SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                qrByTable_[t] = { tex, w, h, true };
            }
        }
    }

    // Use fixed (baseline) grid for geometry + per-row scale
    const int   cols = gridBaselineCols_;
    const int   rows = gridBaselineRows_;
    const float cellW = gridBaselineCellW_;
    const float cellH = gridBaselineCellH_;
    const float spacingH = cellSpacingH_ * compW;
    const float spacingV = cellSpacingV_ * compH;

    auto quantize = [](float s) { const float q = 64.f; return std::max(0.f, std::round(s * q) / q); };

    planned_.assign(Nvisible, {});
    tablePanels_.resize(Nvisible);

    for (int t = 0; t < Nvisible; ++t) {
        const int globalIdx = startIdx + t;
        const auto& table = highScoreTable_->tables[globalIdx];

        const int slotRow = (t / cols);                     // row within the fixed grid
        float finalScale = quantize(baseScale * gridBaselineRowMin_[std::min(slotRow, rows - 1)]);

        const float drawableH = asc * finalScale;
        const float lineH = drawableH * (1.0f + baseRowPadding_);
        const float colPad = baseColumnPadding_ * drawableH;

        // Measure columns at the now-fixed finalScale
        std::vector<float> colW(table.columns.size(), 0.0f);
        float totalWCols = 0.0f;
        for (size_t c = 0; c < table.columns.size(); ++c) {
            float w = (float)font->getWidth(table.columns[c]) * finalScale;
            for (const auto& rowV : table.rows)
                if (c < rowV.size()) w = std::max(w, (float)font->getWidth(rowV[c]) * finalScale);
            colW[c] = w;
            totalWCols += w;
            if (c + 1 < table.columns.size()) totalWCols += colPad;
        }

        float titleH = table.id.empty() ? 0.0f : lineH;
        float headerH = lineH;
        float dataH = lineH * kRowsPerPage;
        const float textBlockHeight = titleH + headerH + dataH;

        // Make the panel texture
        int pageW = std::max(1, (int)std::ceil(totalWCols));
        int pageH = std::max(1, (int)std::ceil(textBlockHeight));

        PageTex pt;
        pt.tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, pageW, pageH);
        pt.w = pageW; pt.h = pageH;
        SDL_SetTextureBlendMode(pt.tex, SDL_BLENDMODE_BLEND);

        SDL_Texture* old = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, pt.tex);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        float y = 0.0f;

        // Title centered
        if (!table.id.empty()) {
            int titleWpx = (int)std::lround(font->getWidth(table.id) * finalScale);
            int totalWpx = (int)std::lround(totalWCols);
            float x = std::round((float)((totalWpx - titleWpx) / 2));
            renderTextOutlined(renderer, font, table.id, x, y, finalScale);
            y += lineH;
        }

        // Headers centered within each column
        {
            float x = 0.0f;
            for (size_t c = 0; c < table.columns.size(); ++c) {
                const std::string& header = table.columns[c];
                float hw = (float)font->getWidth(header) * finalScale;
                int   hwpx = (int)std::lround(hw);
                int   colWpx = (int)std::lround(colW[c]);
                float xAligned = std::round(x + (colWpx - hwpx) * 0.5f);
                renderTextOutlined(renderer, font, header, xAligned, y, finalScale);
                x += colW[c];
                if (c + 1 < table.columns.size()) x += colPad;
            }
            y += lineH;
        }

        // Rows
        auto colAlignFor = [](size_t idx, size_t nCols) {
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
                case ColAlign::Left:   return x;
                case ColAlign::Center: return x + (colW - textW) * 0.5f;
                case ColAlign::Right:  return x + (colW - textW);
            }
            return x;
            };

        for (int r = 0; r < kRowsPerPage; ++r) {
            float x = 0.0f;
            const auto& rowV = table.rows[(size_t)r];
            for (size_t c = 0; c < table.columns.size(); ++c) {
                const std::string cell = (c < rowV.size()) ? rowV[c] : std::string();
                const float cw = (float)font->getWidth(cell) * finalScale;
                const bool ph = isPlaceholderCell(cell);
                const ColAlign a = ph ? ColAlign::Center : colAlignFor(c, table.columns.size());
                const float xAligned = alignX(x, colW[c], cw, a);
                renderTextOutlined(renderer, font, cell, xAligned, y, finalScale);
                x += colW[c];
                if (c + 1 < table.columns.size()) x += colPad;
            }
            y += lineH;
        }

#ifndef NDEBUG
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        SDL_Rect outline = { 0, 0, pt.w, pt.h };
        SDL_RenderDrawRect(renderer, &outline);
#endif
        SDL_SetRenderTarget(renderer, old);
        tablePanels_[t] = pt;

        // --- anchor inside fixed grid cell (same positions as page 0) ---
        int extraL = 0, extraR = 0, extraT = 0, extraB = 0;
        if (t < (int)qrByTable_.size() && qrByTable_[t].ok) {
            const auto& q = qrByTable_[t];
            switch (qrPlacement_) {
                case QrPlacement::TopCentered:   extraT = qrMarginPx_ + q.h; break;
                case QrPlacement::BottomCenter:  extraB = qrMarginPx_ + q.h; break;
                case QrPlacement::TopRight:
                case QrPlacement::RightMiddle:   extraR = qrMarginPx_ + q.w; break;
                case QrPlacement::TopLeft:
                case QrPlacement::LeftMiddle:    extraL = qrMarginPx_ + q.w; break;
                case QrPlacement::BottomRight:   extraR = qrMarginPx_ + q.w; extraB = qrMarginPx_ + q.h; break;
                case QrPlacement::BottomLeft:    extraL = qrMarginPx_ + q.w; extraB = qrMarginPx_ + q.h; break;
                default: break;
            }
        }

        const float xCell = (t % cols) * (cellW + spacingH);
        const float yCell = (t / cols) * (cellH + spacingV);

        const float anchorW = (float)pt.w + (float)(extraL + extraR);
        const float anchorH = (float)pt.h + (float)(extraT + extraB);

        auto clamp = [](float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); };
        float anchorX = xCell + (cellW - anchorW) * 0.5f;
        float anchorY = yCell;
        anchorX = std::round(clamp(anchorX, xCell, xCell + cellW - anchorW));
        anchorY = std::round(clamp(anchorY, yCell, yCell + cellH - anchorH));

        PlannedDraw pd;
        pd.dst = { anchorX + (float)extraL, anchorY + (float)extraT, (float)pt.w, (float)pt.h };
        pd.headerTopLocal = titleH;
        pd.anchorX = anchorX; pd.anchorY = anchorY;
        pd.anchorW = anchorW; pd.anchorH = anchorH;
        pd.extraL = extraL; pd.extraR = extraR; pd.extraT = extraT; pd.extraB = extraB;
        planned_[t] = pd;
    }

    needsRedraw_ = true;
}

void ReloadableGlobalHiscores::draw() {
    Component::draw();

    if (baseViewInfo.Alpha <= 0.0f) return;
    if (!(highScoreTable_ && !highScoreTable_->tables.empty())) return;
    if (tablePanels_.empty() || planned_.empty()) return;

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (!renderer) return;

    const int compositeW = (int)std::lround(baseViewInfo.Width);
    const int compositeH = (int)std::lround(baseViewInfo.Height);
    if (compositeW <= 0 || compositeH <= 0) return;

    static int prevW = 0, prevH = 0;
    const bool sizeChanged = (!intermediateTexture_) || prevW != compositeW || prevH != compositeH;
    if (sizeChanged) {
        if (intermediateTexture_) SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, compositeW, compositeH);
        if (!intermediateTexture_) return;
        SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
        prevW = compositeW; prevH = compositeH;
        needsRedraw_ = true;
    }

    if (needsRedraw_) {
        SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, intermediateTexture_);

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        const int Nvisible = (int)std::min(tablePanels_.size(), planned_.size());

        // 1) Panels
        for (int i = 0; i < Nvisible; ++i) {
            const PageTex& pt = tablePanels_[i];
            if (!pt.tex) continue;
            const SDL_FRect& dst = planned_[i].dst;
            SDL_RenderCopyF(renderer, pt.tex, nullptr, &dst);
        }

        // 2) QRs (positioned relative to each panel rect)
        if (!qrByTable_.empty() && Nvisible > 0) {
            const int Mqr = (int)std::min({ qrByTable_.size(), tablePanels_.size(), planned_.size() });
            for (int i = 0; i < Mqr; ++i) {
                const QrEntry& q = qrByTable_[i];
                if (!q.ok || !q.tex) continue;

                const SDL_FRect& panel = planned_[i].dst;
                float qrX = 0.f, qrY = 0.f;

                switch (qrPlacement_) {
                    case QrPlacement::TopCentered:
                    qrX = panel.x + (panel.w - (float)q.w) * 0.5f;
                    qrY = panel.y - (float)qrMarginPx_ - (float)q.h;
                    break;
                    case QrPlacement::BottomCenter:
                    qrX = panel.x + (panel.w - (float)q.w) * 0.5f;
                    qrY = panel.y + panel.h + (float)qrMarginPx_;
                    break;
                    case QrPlacement::TopRight:
                    qrX = panel.x + panel.w + (float)qrMarginPx_;
                    qrY = panel.y + (float)qrMarginPx_;
                    break;
                    case QrPlacement::TopLeft:
                    qrX = panel.x - (float)qrMarginPx_ - (float)q.w;
                    qrY = panel.y + planned_[i].headerTopLocal + (float)qrMarginPx_;
                    break;
                    case QrPlacement::BottomRight:
                    qrX = panel.x + panel.w + (float)qrMarginPx_;
                    qrY = panel.y + panel.h - (float)q.h - (float)qrMarginPx_;
                    break;
                    case QrPlacement::BottomLeft:
                    qrX = panel.x - (float)qrMarginPx_ - (float)q.w;
                    qrY = panel.y + panel.h - (float)q.h - (float)qrMarginPx_;
                    break;
                    case QrPlacement::RightMiddle:
                    qrX = panel.x + panel.w + (float)qrMarginPx_;
                    qrY = panel.y + (panel.h - (float)q.h) * 0.5f;
                    break;
                    case QrPlacement::LeftMiddle:
                    qrX = panel.x - (float)qrMarginPx_ - (float)q.w;
                    qrY = panel.y + (panel.h - (float)q.h) * 0.5f;
                    break;
                    default: // BottomCenter
                    qrX = panel.x + (panel.w - (float)q.w) * 0.5f;
                    qrY = panel.y + panel.h + (float)qrMarginPx_;
                    break;
                }

                if (fontInst_) {
                    const SDL_Color c = fontInst_->getColor();
                    Uint8 maxCh = std::max({ c.r, c.g, c.b });
                    SDL_SetTextureColorMod(q.tex, maxCh, maxCh, maxCh);
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

        SDL_SetRenderTarget(renderer, oldTarget);
        needsRedraw_ = false;
    }

    // Final blit with component transform
    SDL_FRect rect = {
        baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
        baseViewInfo.ScaledWidth(),       baseViewInfo.ScaledHeight()
    };
    SDL::renderCopyF(
        intermediateTexture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor)
    );
}


void ReloadableGlobalHiscores::destroyAllQr_() {
    for (auto& q : qrByTable_) {
        if (q.tex) SDL_DestroyTexture(q.tex);
        q = {};
    }
    qrByTable_.clear();
}