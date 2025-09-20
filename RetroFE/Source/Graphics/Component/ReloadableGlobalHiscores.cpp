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

namespace {
    static inline std::string makeOrdinal_(int n) {
        int x = n % 100;
        if (x >= 11 && x <= 13) return std::to_string(n) + "th";
        switch (n % 10) {
            case 1: return std::to_string(n) + "st";
            case 2: return std::to_string(n) + "nd";
            case 3: return std::to_string(n) + "rd";
            default: return std::to_string(n) + "th";
        }
    }
    static inline bool isTimeHeader_(const std::string& s) {
        return Utils::toLower(s).rfind("time", 0) == 0; // starts with "time"
    }
} // namespace

ReloadableGlobalHiscores::~ReloadableGlobalHiscores() {
    freeGraphicsMemory();
}

bool ReloadableGlobalHiscores::update(float dt) {
    (void)dt; // no per-frame timers in grid mode

    // Detect selection or layout changes -> rebuild once
    Item* selectedItem = page.getSelectedItem(displayOffset_);
    bool itemChanged = (selectedItem != lastSelectedItem_);

    float widthNow = baseViewInfo.Width;
    float heightNow = baseViewInfo.Height;
    static float prevW = -1.0f, prevH = -1.0f, prevFont = -1.0f;

    bool geomChanged = (prevW != widthNow || prevH != heightNow || prevFont != baseViewInfo.FontSize);

    if (itemChanged || geomChanged || !highScoreTable_) {
        lastSelectedItem_ = selectedItem;
        reloadTexture(true);
        prevW = widthNow; prevH = heightNow; prevFont = baseViewInfo.FontSize;
    }

    return Component::update(dt);
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

// No longer need to add #include <sstream>

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

    enum class ColAlign { Left, Center, Right };

    auto colAlignFor = [](size_t idx, size_t nCols) -> ColAlign {
        // Your tables are built as: [0]=Rank, [1]=Name, [2]=Score/Time/Cash, [3]=Date
        if (nCols >= 4) {
            if (idx == 0) return ColAlign::Left;   // Rank
            if (idx == 1) return ColAlign::Center;    // Name
            if (idx == 2) return ColAlign::Right;   // Score/Time/Cash
            if (idx == 3) return ColAlign::Right;  // Date (or Right if you prefer)
        }
        return ColAlign::Center; // fallback for odd tables
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

    // Clear previous panel textures
    for (auto& p : tablePanels_) {
        if (p.tex) { SDL_DestroyTexture(p.tex); p.tex = nullptr; }
    }
    tablePanels_.clear();

    // Clear previous QRs
    for (auto& q : qrByTable_) {
        if (q.tex) SDL_DestroyTexture(q.tex);
    }
    qrByTable_.clear();

    // Fetch tables for selected item
    Item* selectedItem = page.getSelectedItem(displayOffset_);
    if (!selectedItem || !renderer) {
        highScoreTable_ = nullptr;
        needsRedraw_ = true;
        return;
    }

    highScoreTable_ = HiScores::getInstance().getGlobalHiScoreTable(selectedItem);
    if (!highScoreTable_ || highScoreTable_->tables.empty()) {
        needsRedraw_ = true;
        return;
    }

    // Parse comma-separated game IDs (index-aligned with tables)
    std::vector<std::string> gameIds;
    if (!selectedItem->iscoredId.empty()) {
        Utils::listToVector(selectedItem->iscoredId, gameIds, ',');
    }

    const int N = (int)highScoreTable_->tables.size();

    // Preload per-table QR textures (kept for final-screen draw)
    qrByTable_.resize(N);
    for (int t = 0; t < N; ++t) {
        if (t < (int)gameIds.size() && !gameIds[t].empty()) {
            const std::string path = Configuration::absolutePath + "/iScored/qr/" + gameIds[t] + ".png";
            if (SDL_Texture* tex = IMG_LoadTexture(renderer, path.c_str())) {
                int w = 0, h = 0; SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                // Get the font color (Uint8 components)
                SDL_Color ink = fontInst_ ? fontInst_->getColor() : SDL_Color{ 255,255,255,255 };
                SDL_SetTextureColorMod(tex, ink.r, ink.g, ink.b);
                qrByTable_[t] = { tex, w, h, true };
            }
            else {
                qrByTable_[t] = { nullptr, 0, 0, false };
            }
        }
        else {
            qrByTable_[t] = { nullptr, 0, 0, false };
        }
    }

    // Grid geometry
    const float compW = baseViewInfo.Width;
    const float compH = baseViewInfo.Height;

    int cols = gridColsHint_ > 0 ? gridColsHint_ : (int)std::ceil(std::sqrt((double)N));
    cols = std::max(1, cols);
    int rows = (N + cols - 1) / cols;

    const float spacingH = cellSpacingH_ * compW;
    const float spacingV = cellSpacingV_ * compH;

    const float totalW = compW - spacingH * (cols - 1);
    const float totalH = compH - spacingV * (rows - 1);
    const float cellW = totalW / cols;
    const float cellH = totalH / rows;

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font) { needsRedraw_ = true; return; }

    const float baseScale = baseViewInfo.FontSize / (float)font->getHeight();
    const float asc = (float)font->getAscent();

    // PASS 1: measure at baseScale, compute downscale to fit cell (reserving for QR as needed)
    std::vector<float> needScale(N, 1.0f);

    for (int t = 0; t < N; ++t) {
        const auto& table = highScoreTable_->tables[t];

        const float drawableH0 = asc * baseScale;
        const float lineH0 = drawableH0 * (1.0f + baseRowPadding_);
        const float colPad0 = baseColumnPadding_ * drawableH0;

        float width0 = 0.0f;
        for (size_t c = 0; c < table.columns.size(); ++c) {
            float w = (float)font->getWidth(table.columns[c]) * baseScale;
            for (const auto& row : table.rows) {
                if (c < row.size()) w = std::max(w, (float)font->getWidth(row[c]) * baseScale);
            }
            width0 += w;
            if (c + 1 < table.columns.size()) width0 += colPad0;
        }

        float height0 = lineH0;                   // header
        if (!table.id.empty()) height0 += lineH0; // title
        height0 += lineH0 * kRowsPerPage;         // rows

        // --- Reserve cell space for QR, depending on placement ---
        if (t < (int)qrByTable_.size() && qrByTable_[t].ok) {
            const int qW = qrByTable_[t].w;
            const int qH = qrByTable_[t].h;

            auto reserveW = [&](int w) { width0 += (float)(qrMarginPx_ + w); };
            auto reserveH = [&](int h) { height0 += (float)(qrMarginPx_ + h); };
            auto ensureH = [&](int h) { height0 = std::max(height0, (float)(h + 2 * qrMarginPx_)); };

            switch (qrPlacement_) {
                case QrPlacement::TopCentered:
                reserveH(qH);                       // above panel
                break;
                case QrPlacement::BottomCenter:
                reserveH(qH);                       // below panel
                break;
                case QrPlacement::TopRight:
                reserveW(qW); ensureH(qH);          // right of panel, align to top
                break;
                case QrPlacement::TopLeft:
                reserveW(qW); ensureH(qH);          // left of panel, align to top
                break;
                case QrPlacement::BottomRight:
                reserveW(qW); reserveH(qH);         // right & below
                break;
                case QrPlacement::BottomLeft:
                reserveW(qW); reserveH(qH);         // left & below
                break;
                case QrPlacement::RightMiddle:
                reserveW(qW); ensureH(qH);          // right, vertically centered
                break;
                case QrPlacement::LeftMiddle:
                reserveW(qW); ensureH(qH);          // left, vertically centered
                break;
                default: break;
            }
        }

        const float sW = width0 > 0 ? (cellW / width0) : 1.0f;
        const float sH = height0 > 0 ? (cellH / height0) : 1.0f;
        needScale[t] = std::min({ 1.0f, sW, sH }); // never upscale
    }

    // PASS 2: row-uniform scale
    std::vector<float> rowMin(rows, 1.0f);
    for (int r = 0; r < rows; ++r) {
        float s = 1.0f;
        for (int c = 0; c < cols; ++c) {
            int i = r * cols + c;
            if (i >= N) break;
            s = std::min(s, needScale[i]);
        }
        rowMin[r] = s;
    }
    auto quantize = [](float s) { const float q = 64.f; return std::max(0.f, std::round(s * q) / q); };

    planned_.assign(N, {});
    tablePanels_.resize(N);

    // PASS 3: build panel textures (text only; QR drawn later in screen space)
    for (int t = 0; t < N; ++t) {
        const auto& table = highScoreTable_->tables[t];

        int rowIdx = t / cols;
        float finalScale = quantize(baseScale * rowMin[rowIdx]);

        const float drawableH = asc * finalScale;
        const float lineH = drawableH * (1.0f + baseRowPadding_);
        const float colPad = baseColumnPadding_ * drawableH;

        // column widths at final scale
        std::vector<float> colW(table.columns.size(), 0.0f);
        float totalWCols = 0.0f;
        for (size_t c = 0; c < table.columns.size(); ++c) {
            float w = (float)font->getWidth(table.columns[c]) * finalScale;
            for (const auto& rowV : table.rows) {
                if (c < rowV.size()) w = std::max(w, (float)font->getWidth(rowV[c]) * finalScale);
            }
            colW[c] = w;
            totalWCols += w;
            if (c + 1 < table.columns.size()) totalWCols += colPad;
        }

        float titleH = table.id.empty() ? 0.0f : lineH;
        float headerH = lineH;
        float dataH = lineH * kRowsPerPage;
        const float textBlockHeight = titleH + headerH + dataH;

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

        // Title
        if (!table.id.empty()) {
            int titleWpx = (int)std::lround(font->getWidth(table.id) * finalScale);
            int totalWpx = (int)std::lround(totalWCols);
            float x = std::round((float)((totalWpx - titleWpx) / 2));
            renderTextWithKerning(renderer, font, table.id, x, y, finalScale);
            y += lineH;
        }

        // Headers (center per column)
        {
            float x = 0.0f;
            for (size_t c = 0; c < table.columns.size(); ++c) {
                const std::string& header = table.columns[c];
                float hw = (float)font->getWidth(header) * finalScale;
                int   hwpx = (int)std::lround(hw);
                int   colWpx = (int)std::lround(colW[c]);
                float xAligned = std::round(x + (colWpx - hwpx) * 0.5f);
                renderTextWithKerning(renderer, font, header, xAligned, y, finalScale);
                x += colW[c];
                if (c + 1 < table.columns.size()) x += colPad;
            }
            y += lineH;
        }

        // Data rows (center per column)
        for (int r = 0; r < kRowsPerPage; ++r) {
            float x = 0.0f;
            const auto& rowV = table.rows[(size_t)r];
            for (size_t c = 0; c < table.columns.size(); ++c) {
                const std::string cell = (c < rowV.size()) ? rowV[c] : std::string();
                const float cw = (float)font->getWidth(cell) * finalScale;

                const ColAlign a = colAlignFor(c, table.columns.size());
                const float xAligned = alignX(x, colW[c], cw, a);

                renderTextWithKerning(renderer, font, cell, xAligned, y, finalScale);

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

        // Center the panel (QR positioning is handled later; doesn't affect centering)
        const float xCell = (t % cols) * (cellW + spacingH);
        const float yCell = (t / cols) * (cellH + spacingV);
        planned_[t].dst = {
            std::round(xCell + (cellW - (float)pt.w) * 0.5f),
            std::round(yCell),
            (float)pt.w, (float)pt.h
        };
		planned_[t].headerTopLocal = titleH; // relative to panel top
}

    needsRedraw_ = true;
}

void ReloadableGlobalHiscores::draw() {
    Component::draw();

    // Nothing to draw?
    if (!(highScoreTable_ && !highScoreTable_->tables.empty()) || baseViewInfo.Alpha <= 0.0f)
        return;
    if (tablePanels_.empty() || planned_.empty())
        return;

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (!renderer) return;

    // The intermediate canvas is the component’s local pixel size.
    const int compositeW = (int)std::round(baseViewInfo.Width);
    const int compositeH = (int)std::round(baseViewInfo.Height);
    if (compositeW <= 0 || compositeH <= 0) return;

    // Ensure/resize intermediate texture
    static int prevW = 0, prevH = 0;
    const bool sizeChanged = (!intermediateTexture_) || prevW != compositeW || prevH != compositeH;
    if (sizeChanged) {
        if (intermediateTexture_) SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, compositeW, compositeH);
        if (!intermediateTexture_) return;
        SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
        prevW = compositeW; prevH = compositeH;
        needsRedraw_ = true; // new size: redraw contents
    }

    // Redraw the composite when needed.
    if (needsRedraw_) {
        SDL_Texture* oldTarget = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, intermediateTexture_);

        // Clear to transparent
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        // --- 1) Draw all table panels (already sized & positioned in component-local coords) ---
        const int N = (int)std::min(tablePanels_.size(), planned_.size());
        for (int i = 0; i < N; ++i) {
            const PageTex& pt = tablePanels_[i];
            if (!pt.tex) continue;
            const SDL_FRect& dst = planned_[i].dst; // component-local destination
            SDL_RenderCopyF(renderer, pt.tex, nullptr, &dst);
        }

        // --- 2) Draw QRs into the same intermediate (native size; never scaled with table) ---
        //      Position is computed *relative to the table rect* in component-local space,
        //      so QRs "ride along" without affecting table centering or scaling.
        if (!qrByTable_.empty()) {
            const int M = (int)std::min(qrByTable_.size(), tablePanels_.size());
            for (int i = 0; i < M; ++i) {
                const QrEntry& q = qrByTable_[i];
                if (!q.ok || !q.tex) continue;

                const SDL_FRect& panel = planned_[i].dst; // table destination in component-local coords
                float qrLocalX = 0.f, qrLocalY = 0.f;

                switch (qrPlacement_) {
                    case QrPlacement::TopCentered:
                    qrLocalX = panel.x + (panel.w - (float)q.w) * 0.5f;
                    qrLocalY = panel.y - (float)qrMarginPx_ - (float)q.h;
                    break;
                    case QrPlacement::BottomCenter:
                    qrLocalX = panel.x + (panel.w - (float)q.w) * 0.5f;
                    qrLocalY = panel.y + panel.h + (float)qrMarginPx_;
                    break;
                    case QrPlacement::TopRight:
                    qrLocalX = panel.x + panel.w + (float)qrMarginPx_;
                    qrLocalY = panel.y + (float)qrMarginPx_;
                    break;
                    case QrPlacement::TopLeft:
                    qrLocalX = panel.x - (float)qrMarginPx_ - (float)q.w;
                    qrLocalY = panel.y + +planned_[i].headerTopLocal + (float)qrMarginPx_;
                    break;
                    case QrPlacement::BottomRight:
                    qrLocalX = panel.x + panel.w + (float)qrMarginPx_;
                    qrLocalY = panel.y + panel.h - (float)q.h - (float)qrMarginPx_;
                    break;
                    case QrPlacement::BottomLeft:
                    qrLocalX = panel.x - (float)qrMarginPx_ - (float)q.w;
                    qrLocalY = panel.y + panel.h - (float)q.h - (float)qrMarginPx_;
                    break;
                    case QrPlacement::RightMiddle:
                    qrLocalX = panel.x + panel.w + (float)qrMarginPx_;
                    qrLocalY = panel.y + (panel.h - (float)q.h) * 0.5f;
                    break;
                    case QrPlacement::LeftMiddle:
                    qrLocalX = panel.x - (float)qrMarginPx_ - (float)q.w;
                    qrLocalY = panel.y + (panel.h - (float)q.h) * 0.5f;
                    break;
                    default: // fallback: BottomCenter
                    qrLocalX = panel.x + (panel.w - (float)q.w) * 0.5f;
                    qrLocalY = panel.y + panel.h + (float)qrMarginPx_;
                    break;
                }

                // Optional tint toward table font color (multiplicative color mod).
                if (fontInst_) {
                    const SDL_Color c = fontInst_->getColor();
                    SDL_SetTextureColorMod(q.tex, c.r, c.g, c.b);
                    SDL_SetTextureAlphaMod(q.tex, c.a);
                    SDL_SetTextureBlendMode(q.tex, SDL_BLENDMODE_BLEND);
                }

                SDL_FRect qrDst = { qrLocalX, qrLocalY, (float)q.w, (float)q.h };
                SDL_RenderCopyF(renderer, q.tex, nullptr, &qrDst);
            }
        }

#ifndef NDEBUG
        // Debug: green outline around the composite
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
    SDL::renderCopyF(intermediateTexture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
}


void ReloadableGlobalHiscores::destroyAllQr_() {
    for (auto& q : qrByTable_) {
        if (q.tex) SDL_DestroyTexture(q.tex);
        q = {};
    }
    qrByTable_.clear();
}