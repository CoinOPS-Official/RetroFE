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

 // ============================================================================
 // Constructor / Destructor
 // ============================================================================

    ReloadableGlobalHiscores::ReloadableGlobalHiscores(
        Configuration& /*config*/,
        std::string textFormat,
        Page& p,
        int displayOffset,
        FontManager* font,
        float baseColumnPadding,
        float baseRowPadding)
    : Component(p)
    , fontInst_(font)
    , textFormat_(std::move(textFormat))
    , baseColumnPadding_(baseColumnPadding)
    , baseRowPadding_(baseRowPadding)
    , displayOffset_(displayOffset)
    , needsRedraw_(true)
    , highScoreTable_(nullptr)
    , intermediateTexture_(nullptr)
    , prevCompositeTexture_(nullptr)
    , prevGeomW_(-1.f)
    , prevGeomH_(-1.f)
    , prevGeomFont_(-1.f)
    , compW_(0)
    , compH_(0)
    , gridColsHint_(0)
    , cellSpacingH_(0.02f)
    , cellSpacingV_(0.02f)
    , gridPageSize_(6)
    , gridRotatePeriodSec_(8.0f)
    , gridTimerSec_(0.f)
    , gridPageIndex_(0)
    , gridBaselineSlots_(0)
    , gridBaselineCols_(0)
    , gridBaselineRows_(0)
    , gridBaselineCellW_(0.f)
    , gridBaselineCellH_(0.f)
    , gridBaselineValid_(false)
    , reloadDebounceTimer_(0.f)
    , reloadDebounceSec_(0.08f)
    , pagePhase_(PagePhase::Single)
    , pageT_(0.f)
    , pageDurationSec_(1.0f)
    , qrPhase_(QrPhase::Hidden)
    , qrT_(0.f)
    , qrDelaySec_(2.0f)
    , qrFadeSec_(1.0f)
    , qrPlacement_(QrPlacement::TopLeft)
    , qrMarginPx_(6) {
}

ReloadableGlobalHiscores::~ReloadableGlobalHiscores() {
    freeGraphicsMemory();
}

// ============================================================================
// State Helper Functions
// ============================================================================

void ReloadableGlobalHiscores::beginContext_() {
    // New game/score context - complete reset
    pagePhase_ = PagePhase::Single;
    pageT_ = 0.f;

    // QR re-arms the one-time delay on first visibility
    qrPhase_ = QrPhase::Hidden;
    qrT_ = 0.f;

    // Reset grid state
    gridPageIndex_ = 0;
    gridTimerSec_ = 0.f;
    gridBaselineValid_ = false;

    // Mark for redraw
    needsRedraw_ = true;
    reloadDebounceTimer_ = reloadDebounceSec_;

    // Clean up any old crossfade artifacts
    if (prevCompositeTexture_) {
        SDL_DestroyTexture(prevCompositeTexture_);
        prevCompositeTexture_ = nullptr;
    }
    destroyPrevQr_();
}

void ReloadableGlobalHiscores::beginPageFlip_() {
    // Request crossfade - snapshot will happen in reloadTexture()
    pagePhase_ = PagePhase::SnapshotPending;  // ? CHANGED
    pageT_ = 0.f;

    // IMPORTANT: Don't reset QR phase here
    // - If already Visible: stays Visible (no re-delay on page flips)
    // - If FadingIn: continues its fade
    // - If WaitingDelay: continues waiting (rare edge case)

    // Mark for redraw
    needsRedraw_ = true;
    reloadDebounceTimer_ = reloadDebounceSec_;
}

void ReloadableGlobalHiscores::snapshotPrevPage_(SDL_Renderer* r, int compositeW, int compositeH) {
    // If we already have a snapshot, don't recreate
    if (prevCompositeTexture_) return;

    // Safety check - need something to snapshot
    if (!intermediateTexture_) return;

    // 1) Create texture to hold previous composite
    prevCompositeTexture_ = SDL_CreateTexture(
        r,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_TARGET,
        compositeW,
        compositeH
    );

    if (!prevCompositeTexture_) {
        Logger::write(Logger::ZONE_ERROR, "ReloadableGlobalHiscores",
            "Failed to create prevCompositeTexture_");
        return;
    }

    SDL_SetTextureBlendMode(prevCompositeTexture_, SDL_BLENDMODE_BLEND);

    // 2) Copy current composite into prev
    SDL_Texture* oldRT = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, prevCompositeTexture_);
    SDL_RenderCopy(r, intermediateTexture_, nullptr, nullptr);
    SDL_SetRenderTarget(r, oldRT);

    // 3) Move current QRs ? prev (so they fade out with old page)
    destroyPrevQr_();
    prevQrByTable_ = std::move(qrByTable_);
    qrByTable_.clear();

    // 4) Transition from SnapshotPending > Crossfading NOW
    if (pagePhase_ == PagePhase::SnapshotPending) {
        pagePhase_ = PagePhase::Crossfading;
        pageT_ = 0.f;  // Start the fade timer NOW
    }
}

void ReloadableGlobalHiscores::updateState_(float dt, bool visible) {
    // QR state machine only (page crossfade moved to update())
    if (!visible) {
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;
        return;
    }

    // Component is visible, advance QR state
    switch (qrPhase_) {
        case QrPhase::Hidden:
        qrPhase_ = QrPhase::WaitingDelay;
        qrT_ = 0.f;
        break;

        case QrPhase::WaitingDelay:
        qrT_ += dt;
        if (qrT_ >= qrDelaySec_) {
            qrPhase_ = QrPhase::FadingIn;
            qrT_ = 0.f;
        }
        break;

        case QrPhase::FadingIn:
        qrT_ += dt;
        if (qrT_ >= qrFadeSec_) {
            qrPhase_ = QrPhase::Visible;
            qrT_ = qrFadeSec_;
        }
        break;

        case QrPhase::Visible:
        // Steady state
        break;
    }
}

void ReloadableGlobalHiscores::computeAlphas_(
    float baseA,
    float& prevCompA,
    float& newCompA,
    float& prevQrA,
    float& newQrA) const {
    // Compute crossfade interpolation factor
    float t = (pageDurationSec_ > 0.f) ? (pageT_ / pageDurationSec_) : 1.f;
    t = std::clamp(t, 0.f, 1.f);

    // --- Composite alphas ---
    if (pagePhase_ == PagePhase::Crossfading) {
        prevCompA = baseA * (1.f - t);
        newCompA = baseA * t;
    }
    else {
        // Single OR SnapshotPending > show current composite at full alpha
        prevCompA = 0.f;
        newCompA = baseA;
    }

    // --- QR phase factor ---
    float q = 0.f;
    switch (qrPhase_) {
        case QrPhase::Hidden:
        case QrPhase::WaitingDelay:
        q = 0.f;
        break;

        case QrPhase::FadingIn:
        q = (qrFadeSec_ > 0.f)
            ? std::clamp(qrT_ / qrFadeSec_, 0.f, 1.f)
            : 1.f;
        break;

        case QrPhase::Visible:
        q = 1.f;
        break;
    }

    // --- QR alphas ---
    if (pagePhase_ == PagePhase::Crossfading) {
        // Old QRs fade out with old page
        prevQrA = baseA * (1.f - t);

        // New QRs: if first-time fade is active, apply both page fade and QR fade
        // Otherwise just ride the page crossfade
        const bool firstFadeActive = (qrPhase_ == QrPhase::FadingIn ||
            qrPhase_ == QrPhase::WaitingDelay);
        const float blend = firstFadeActive ? (t * q) : t;
        newQrA = baseA * blend;
    }
    else {
        // Single OR SnapshotPending
        prevQrA = 0.f;
        newQrA = baseA * q;
    }
}

// ============================================================================
// Resource Management
// ============================================================================

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

    if (prevCompositeTexture_) {
        SDL_DestroyTexture(prevCompositeTexture_);
        prevCompositeTexture_ = nullptr;
    }

    destroyAllQr_();
    destroyPrevQr_();
}

void ReloadableGlobalHiscores::destroyAllQr_() {
    for (auto& q : qrByTable_) {
        if (q.tex) {
            SDL_DestroyTexture(q.tex);
        }
    }
    qrByTable_.clear();
}

void ReloadableGlobalHiscores::destroyPrevQr_() {
    for (auto& q : prevQrByTable_) {
        if (q.tex) {
            SDL_DestroyTexture(q.tex);
        }
    }
    prevQrByTable_.clear();
}

void ReloadableGlobalHiscores::deInitializeFonts() {
    if (fontInst_) {
        fontInst_->deInitialize();
    }
}

void ReloadableGlobalHiscores::initializeFonts() {
    if (fontInst_) {
        fontInst_->initialize();
    }
}

// ============================================================================
// Signature & Change Detection
// ============================================================================

// FNV-1a 64-bit hash helpers
static inline void fnv1a_mix(uint64_t& h, const char* p, size_t n) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME = 1099511628211ull;
    if (h == 0) h = FNV_OFFSET;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)p[i];
        h *= FNV_PRIME;
    }
}

static inline std::vector<std::string> splitCsvIds_(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        }
        else if (c > ' ') {
            cur.push_back(c);  // simple trim
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

// Returns {startIndex, count} for the current page
static inline std::pair<int, int> computeVisibleRange(
    int totalTables,
    int pageIndex,
    int pageSize) {
    if (pageSize <= 0) pageSize = 6;
    if (totalTables <= 0) return { 0, 0 };

    const int pageCount = std::max(1, (totalTables + pageSize - 1) / pageSize);
    const int safePage = (pageIndex % pageCount + pageCount) % pageCount;
    const int start = safePage * pageSize;
    const int count = std::min(pageSize, totalTables - start);

    return { start, count };
}

// ============================================================================
// Update
// ============================================================================

bool ReloadableGlobalHiscores::update(float dt) {
    // Sanitize delta time
    if (!std::isfinite(dt)) dt = 0.f;
    dt = std::clamp(dt, 0.f, 0.25f);

    const bool visible = (baseViewInfo.Alpha > 0.0f);

    // --- 1. Update state machines ---
    // Only advance crossfade timer when actually crossfading
    if (pagePhase_ == PagePhase::Crossfading) {
        pageT_ += dt;
        if (pageT_ >= pageDurationSec_) {
            pageT_ = pageDurationSec_;
            pagePhase_ = PagePhase::Single;

            // Cleanup old resources immediately when crossfade completes
            if (prevCompositeTexture_) {
                SDL_DestroyTexture(prevCompositeTexture_);
                prevCompositeTexture_ = nullptr;
            }
            destroyPrevQr_();
        }
    }

    // Update QR state
    if (!visible) {
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;
    }
    else {
        switch (qrPhase_) {
            case QrPhase::Hidden:
            qrPhase_ = QrPhase::WaitingDelay;
            qrT_ = 0.f;
            break;

            case QrPhase::WaitingDelay:
            qrT_ += dt;
            if (qrT_ >= qrDelaySec_) {
                qrPhase_ = QrPhase::FadingIn;
                qrT_ = 0.f;
            }
            break;

            case QrPhase::FadingIn:
            qrT_ += dt;
            if (qrT_ >= qrFadeSec_) {
                qrPhase_ = QrPhase::Visible;
                qrT_ = qrFadeSec_;
            }
            break;

            case QrPhase::Visible:
            // Steady state
            break;
        }
    }

    // --- 2. Debounce countdown ---
    if (reloadDebounceTimer_ > 0.f) {
        reloadDebounceTimer_ -= dt;
    }

    // --- 3. Change detection ---
    const float widthNow = baseViewInfo.Width;
    const float heightNow = baseViewInfo.Height;
    const bool geomChanged =
        (prevGeomW_ != widthNow) ||
        (prevGeomH_ != heightNow) ||
        (prevGeomFont_ != baseViewInfo.FontSize);

    const bool selChange = newItemSelected ||
        (newScrollItemSelected && getMenuScrollReload());

    // Check for data changes
    Item const* selectedItem = page.getSelectedItem(displayOffset_);
    bool dataChanged = false;

    if (selectedItem && !selectedItem->iscoredId.empty()) {
        auto ids = splitCsvIds_(selectedItem->iscoredId);

        // If selection changed, clear old hashes (different games now)
        if (selChange) {
            lastSeenHashes_.clear();
        }

        for (const auto& id : ids) {
            const auto* gg = HiScores::getInstance().getGlobalGameById(id);
            if (!gg) continue;

            auto it = lastSeenHashes_.find(id);
            if (it == lastSeenHashes_.end() || it->second != gg->contentHash) {
                dataChanged = true;
                lastSeenHashes_[id] = gg->contentHash;
            }
        }
    }

    // --- 4. Context change (new game/selection or data update) ---
    if (selChange || dataChanged) {
        beginContext_();
        newItemSelected = false;
        newScrollItemSelected = false;

        if (geomChanged) {
            prevGeomW_ = widthNow;
            prevGeomH_ = heightNow;
            prevGeomFont_ = baseViewInfo.FontSize;
        }

        return Component::update(dt);
    }

    // --- 6. Page rotation ---
    if (highScoreTable_ && !highScoreTable_->tables.empty()) {
        const int totalTables = (int)highScoreTable_->tables.size();
        const int pageSize = std::max(1, gridPageSize_);
        const int pageCount = std::max(1, (totalTables + pageSize - 1) / pageSize);

        if (pageCount > 1) {
            gridTimerSec_ += dt;
            if (gridTimerSec_ >= gridRotatePeriodSec_) {
                gridTimerSec_ = 0.f;
                gridPageIndex_ = (gridPageIndex_ + 1) % pageCount;
                beginPageFlip_();
            }
        }
        else {
            gridTimerSec_ = 0.f;
            gridPageIndex_ = 0;
        }
    }

    return Component::update(dt);
}

// ============================================================================
// Draw
// ============================================================================

void ReloadableGlobalHiscores::draw() {
    Component::draw();
    if (baseViewInfo.Alpha <= 0.0f) return;

    // Rebuild texture if needed (and debounce allows)
    if (needsRedraw_ && reloadDebounceTimer_ <= 0.f) {
        reloadTexture();
    }

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (!renderer || !intermediateTexture_) return;

    // Destination rect for the composite (component space ? screen)
    SDL_FRect rect = {
        baseViewInfo.XRelativeToOrigin(),
        baseViewInfo.YRelativeToOrigin(),
        (float)compW_,
        (float)compH_
    };

    const float baseAlpha = baseViewInfo.Alpha;
    const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
    const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);

    // Compute alphas for all layers
    float prevCompA, newCompA, prevQrA, newQrA;
    computeAlphas_(baseAlpha, prevCompA, newCompA, prevQrA, newQrA);

    // --- Render old page composite (if crossfading) ---
    if (prevCompA > 0.f && prevCompositeTexture_) {
        SDL::renderCopyF(prevCompositeTexture_, prevCompA,
            nullptr, &rect, baseViewInfo, layoutW, layoutH);
    }

    // --- Render new page composite ---
    if (newCompA > 0.f) {
        SDL::renderCopyF(intermediateTexture_, newCompA,
            nullptr, &rect, baseViewInfo, layoutW, layoutH);
    }

    // --- Render old QRs ---
    if (prevQrA > 0.f && !prevQrByTable_.empty()) {
        for (const auto& q : prevQrByTable_) {
            if (!q.ok || !q.tex) continue;
            SDL_FRect dst{
                rect.x + q.dst.x,
                rect.y + q.dst.y,
                q.dst.w,
                q.dst.h
            };
            SDL::renderCopyF(q.tex, prevQrA, nullptr, &dst,
                baseViewInfo, layoutW, layoutH);
        }
    }

    // --- Render new QRs ---
    if (newQrA > 0.f && !qrByTable_.empty()) {
        for (const auto& q : qrByTable_) {
            if (!q.ok || !q.tex) continue;
            SDL_FRect dst{
                rect.x + q.dst.x,
                rect.y + q.dst.y,
                q.dst.w,
                q.dst.h
            };
            SDL::renderCopyF(q.tex, newQrA, nullptr, &dst,
                baseViewInfo, layoutW, layoutH);
        }
    }
}

// ============================================================================
// Grid Baseline Computation
// ============================================================================

void ReloadableGlobalHiscores::computeGridBaseline_(
    FontManager* font,
    int totalTables,
    float compW,
    float compH,
    float baseScale,
    float asc) {
    // Decide number of slots in the grid: gridPageSize_ if we have >= that many tables, else totalTables
    const int slots = (totalTables >= gridPageSize_) ? gridPageSize_ : totalTables;

    // Compute grid geometry from fixed 'slots' (NOT from how many are visible on this page)
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
            for (const auto& row : table.rows) {
                if (c < row.size()) {
                    w = std::max(w, (float)font->getWidth(row[c]) * baseScale);
                }
            }
            width0 += w;
            if (c + 1 < table.columns.size()) {
                width0 += colPad0;
            }
        }

        if (!table.id.empty()) {
            width0 = std::max(width0, (float)font->getWidth(table.id) * baseScale);
        }

        float height0 = lineH0;  // header
        if (!table.id.empty()) {
            height0 += lineH0;  // title
        }
        height0 += lineH0 * kRowsPerPage;  // rows

        const float sW = width0 > 0 ? (cellW / width0) : 1.0f;
        const float sH = height0 > 0 ? (cellH / height0) : 1.0f;
        needScale[t] = std::min({ 1.0f, sW, sH });
    }

    // Per-row minimum over the first-page items
    std::vector<float> rowMin(rows, 1.0f);
    for (int r = 0; r < rows; ++r) {
        float s = 1.0f;
        for (int c = 0; c < cols; ++c) {
            int i = r * cols + c;
            if (i >= firstPageCount) break;
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

// ============================================================================
// Texture Reload (Main Rendering)
// ============================================================================

void ReloadableGlobalHiscores::reloadTexture() {
    // --- Text rendering helper (mipmapped outlined glyphs) ---
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
                float penX = x;
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
                            src.w * k,
                            src.h * k
                        };
                        SDL_RenderCopyF(r, outlineTex, &src, &dst);
                        penX += g.advance * k;
                    }
                    prev = ch;
                }
            }

            // Fill pass
            {
                float penX = x;
                Uint16 prev = 0;
                for (unsigned char uc : s) {
                    Uint16 ch = (Uint16)uc;
                    if (prev) penX += f->getKerning(prev, ch) * finalScale;

                    auto it = mip->glyphs.find(ch);
                    if (it != mip->glyphs.end()) {
                        const auto& g = it->second;
                        SDL_Rect srcFill{
                            g.rect.x + g.fillX,
                            g.rect.y + g.fillY,
                            g.fillW,
                            g.fillH
                        };
                        SDL_FRect dst{
                            penX,
                            ySnap,
                            g.fillW * k,
                            g.fillH * k
                        };
                        SDL_RenderCopyF(r, fillTex, &srcFill, &dst);
                        penX += g.advance * k;
                    }
                    prev = ch;
                }
            }
        };

    // --- Exact width measure (mip + kerning + outline overhang) ---
    auto measureTextWidthExact = [&](FontManager* f, const std::string& s, float scale) -> float {
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

                if (first) {
                    minX = left;
                    maxX = right;
                    first = false;
                }
                else {
                    minX = std::min(minX, left);
                    maxX = std::max(maxX, right);
                }

                penX += g.advance * k;
            }
            prev = ch;
        }

        return std::max(0.0f, maxX - minX);
        };

    // --- Helper: is this cell a placeholder? ---
    auto isPlaceholderCell = [](std::string_view s) -> bool {
        const size_t l = s.find_first_not_of(" \t");
        if (l == std::string_view::npos) return false;
        const size_t r = s.find_last_not_of(" \t");
        s = s.substr(l, r - l + 1);  // trim via view, no copy

        if (s == "-" || s == "$-" || s == "--:--.---") return true;

        for (unsigned char uc : s) {
            char c = static_cast<char>(uc);
            if (c == '-' || c == ':' || c == '.' || c == '$' || c == ' ') continue;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                return false;
            return false;
        }
        return true;
        };

    // --- Column alignment helper ---
    enum class ColAlign { Left, Center, Right };
    auto colAlignFor = [](size_t idx, size_t nCols) -> ColAlign {
        if (nCols >= 4) {
            if (idx == 0) return ColAlign::Left;   // rank
            if (idx == 1) return ColAlign::Left;   // name
            if (idx == 2) return ColAlign::Right;  // score
            if (idx == 3) return ColAlign::Right;  // time
        }
        return ColAlign::Center;
        };

    auto alignX = [](float x, float colW, float textW, ColAlign a) -> float {
        switch (a) {
            case ColAlign::Left:   return x + 1.0f;  // 1px guard
            case ColAlign::Center: return x + (colW - textW) * 0.5f;
            case ColAlign::Right:  return x + (colW - textW);
        }
        return x;
        };

    auto clampf = [](float v, float lo, float hi) {
        return std::max(lo, std::min(v, hi));
        };

    auto quantize_down = [](float s) {
        const float q = 64.f;
        return std::max(0.f, std::floor(s * q) / q);
        };

    // --- Setup / early-outs ---
    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);

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
        needsRedraw_ = false;
        return;
    }

    const int totalTables = (int)highScoreTable_->tables.size();
    const float compW = baseViewInfo.Width;
    const float compH = baseViewInfo.Height;

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font) {
        needsRedraw_ = true;
        return;
    }

    // --- Base metrics ---
    const float baseScale = baseViewInfo.FontSize / (float)font->getMaxHeight();
    const float asc = (float)font->getMaxAscent();

    const float drawableH0 = asc * baseScale;
    const float lineH0 = drawableH0 * (1.0f + baseRowPadding_);
    const float colPad0 = baseColumnPadding_ * drawableH0;

    constexpr float kPanelGuardPx = 1.0f;

    // --- Create/resize composite RT ---
    const int compositeW = (int)std::lround(baseViewInfo.ScaledWidth());
    const int compositeH = (int)std::lround(baseViewInfo.ScaledHeight());
    if (!intermediateTexture_ || compW_ != compositeW || compH_ != compositeH) {
        if (intermediateTexture_) {
            SDL_DestroyTexture(intermediateTexture_);
        }
        intermediateTexture_ = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_TARGET,
            compositeW,
            compositeH
        );
        if (!intermediateTexture_) {
            needsRedraw_ = true;
            return;
        }
        SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
        compW_ = compositeW;
        compH_ = compositeH;
    }

    // --- Snapshot previous page if crossfading ---
    if (pagePhase_ == PagePhase::SnapshotPending) {
        snapshotPrevPage_(renderer, compositeW, compositeH);
        // snapshotPrevPage_() will transition to Crossfading
    }
    else if (pagePhase_ == PagePhase::Single) {
        // Make sure there's no stale prev
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }
        destroyPrevQr_();
    }

    // --- Compute baseline ---
    if (!gridBaselineValid_) {
        computeGridBaseline_(font, totalTables, compW, compH, baseScale, asc);
    }

    const int cols = gridBaselineCols_;
    const int rows = gridBaselineRows_;
    const float cellW = gridBaselineCellW_;
    const float cellH = gridBaselineCellH_;
    const float spacingH = cellSpacingH_ * compW;
    const float spacingV = cellSpacingV_ * compH;

    const auto [startIdx, Nvisible] =
        computeVisibleRange(totalTables, gridPageIndex_, std::max(1, gridPageSize_));
    if (Nvisible <= 0) {
        needsRedraw_ = true;
        return;
    }

    // --- Load QRs for visible set ---
    std::vector<std::string> gameIds;
    if (!selectedItem->iscoredId.empty()) {
        Utils::listToVector(selectedItem->iscoredId, gameIds, ',');
    }

    destroyAllQr_();
    qrByTable_.assign(Nvisible, {});
    for (int t = 0; t < Nvisible; ++t) {
        const int gi = startIdx + t;
        if (gi < (int)gameIds.size() && !gameIds[gi].empty()) {
            std::string path = Configuration::absolutePath + "/iScored/qr/" + gameIds[gi] + ".png";
            if (SDL_Texture* tex = IMG_LoadTexture(renderer, path.c_str())) {
                int w = 0, h = 0;
                SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                qrByTable_[t] = { tex, w, h, true, SDL_FRect{0, 0, 0, 0} };
            }
        }
    }

    // --- QR reservation (size + placement) ---
    struct Margins { float L = 0, R = 0, T = 0, B = 0; };
    std::vector<Margins> qrReserve(Nvisible);
    const bool reserveVerticalForCorners = false;
    const bool reserveHorizontalForSides = true;

    for (int t = 0; t < Nvisible; ++t) {
        if (t >= (int)qrByTable_.size() || !qrByTable_[t].ok) continue;
        const auto& q = qrByTable_[t];
        auto& m = qrReserve[t];

        switch (qrPlacement_) {
            case QrPlacement::TopCentered:
            m.T = (float)qrMarginPx_ + (float)q.h;
            break;
            case QrPlacement::BottomCenter:
            m.B = (float)qrMarginPx_ + (float)q.h;
            break;
            case QrPlacement::LeftMiddle:
            if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)q.w;
            break;
            case QrPlacement::RightMiddle:
            if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)q.w;
            break;
            case QrPlacement::TopLeft:
            if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)q.w;
            if (reserveVerticalForCorners) m.T = (float)qrMarginPx_ + (float)q.h;
            break;
            case QrPlacement::TopRight:
            if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)q.w;
            if (reserveVerticalForCorners) m.T = (float)qrMarginPx_ + (float)q.h;
            break;
            case QrPlacement::BottomLeft:
            if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)q.w;
            if (reserveVerticalForCorners) m.B = (float)qrMarginPx_ + (float)q.h;
            break;
            case QrPlacement::BottomRight:
            if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)q.w;
            if (reserveVerticalForCorners) m.B = (float)qrMarginPx_ + (float)q.h;
            break;
            default:
            break;
        }
    }

    std::vector<float> qrExtraW(Nvisible, 0.0f), qrExtraH(Nvisible, 0.0f);
    for (int t = 0; t < Nvisible; ++t) {
        qrExtraW[t] = qrReserve[t].L + qrReserve[t].R;
        qrExtraH[t] = qrReserve[t].T + qrReserve[t].B;
    }

    // --- Shared column layout (measure exact widths at baseScale) ---
    size_t maxCols = 0;
    for (int t = 0; t < Nvisible; ++t) {
        maxCols = std::max(maxCols, highScoreTable_->tables[startIdx + t].columns.size());
    }
    if (maxCols == 0) {
        needsRedraw_ = true;
        return;
    }

    std::vector<float> maxColW0(maxCols, 0.0f);
    float maxTitleW0 = 0.0f;
    std::vector<float> height0(Nvisible, lineH0 * (1 + kRowsPerPage));

    for (int t = 0; t < Nvisible; ++t) {
        const auto& table = highScoreTable_->tables[startIdx + t];

        for (size_t c = 0; c < maxCols; ++c) {
            float w = 0.0f;
            if (c < table.columns.size()) {
                w = measureTextWidthExact(font, table.columns[c], baseScale);
                for (const auto& row : table.rows) {
                    if (c < row.size()) {
                        w = std::max(w, measureTextWidthExact(font, row[c], baseScale));
                    }
                }
            }
            maxColW0[c] = std::max(maxColW0[c], w);
        }

        if (!table.id.empty()) {
            maxTitleW0 = std::max(maxTitleW0, measureTextWidthExact(font, table.id, baseScale));
        }

        float h = lineH0;  // header
        if (!table.id.empty()) h += lineH0;  // title
        h += lineH0 * kRowsPerPage;  // rows
        height0[t] = h;
    }

    // --- Build exact shared width @ baseScale (columns + pads + guard) ---
    float sumCols0 = 0.0f;
    for (float w : maxColW0) sumCols0 += w;

    float sharedPad0 = colPad0;
    float sharedW0_exact = sumCols0 + (float)(std::max<size_t>(1, maxCols) - 1) * sharedPad0;

    if (sharedW0_exact < maxTitleW0) {
        const int gaps = (int)std::max<size_t>(1, maxCols - 1);
        const float grow0 = maxTitleW0 - sharedW0_exact;
        sharedPad0 += grow0 / (float)gaps;
        sharedW0_exact = maxTitleW0;
    }
    sharedW0_exact += 2.0f * kPanelGuardPx;

    // --- Fit: compute per-slot scale needs using the exact width/height ---
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

    // --- Row-wise min, then quantize DOWN so draw never exceeds the fit ---
    std::vector<float> rowScale(rows, 1.0f);
    for (int r = 0; r < rows; ++r) {
        float s = 1.0f;
        for (int c = 0; c < cols; ++c) {
            int i = r * cols + c;
            if (i >= Nvisible) break;
            s = std::min(s, needScale[i]);
        }
        rowScale[r] = quantize_down(s);
    }

    // --- Paint to composite ---
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
        totalWCols += 2.0f * kPanelGuardPx;

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

        // ---- Title ----
        if (!table.id.empty()) {
            float w = measureTextWidthExact(font, table.id, finalScale);
            float x = std::round((totalWCols - w) * 0.5f);
            renderTextOutlined(renderer, font, table.id, drawX0 + x, y, finalScale);
            y += lineH;
        }

        // ---- Headers ----
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

        // ---- Rows ----
        for (int r = 0; r < kRowsPerPage; ++r) {
            float x = 0.0f;
            const auto* rowV = (r < (int)table.rows.size()) ? &table.rows[r] : nullptr;
            for (size_t c = 0; c < maxCols; ++c) {
                std::string cell;
                if (rowV && c < rowV->size()) cell = (*rowV)[c];
                const float tw = measureTextWidthExact(font, cell, finalScale);
                const bool ph = isPlaceholderCell(cell);
                const ColAlign a = ph ? ColAlign::Center : colAlignFor(c, maxCols);
                const float xAligned = alignX(drawX0 + x, colW[c], tw, a);
                if (!cell.empty()) {
                    renderTextOutlined(renderer, font, cell, std::round(xAligned), y, finalScale);
                }
                x += colW[c];
                if (c + 1 < maxCols) x += colPad;
            }
            y += lineH;
        }

        // ---- QR ----
        if (t < (int)qrByTable_.size() && qrByTable_[t].ok) {
            auto& q = qrByTable_[t];

            SDL_FRect panelRect = {
                drawX0 - kPanelGuardPx,
                anchorY + (float)extraT,
                panelW,
                panelH
            };

            float qrX = 0.f, qrY = 0.f;
            switch (qrPlacement_) {
                case QrPlacement::TopCentered:
                qrX = panelRect.x + (panelRect.w - q.w) * 0.5f;
                qrY = panelRect.y - qrMarginPx_ - q.h;
                break;
                case QrPlacement::BottomCenter:
                qrX = panelRect.x + (panelRect.w - q.w) * 0.5f;
                qrY = panelRect.y + panelRect.h + qrMarginPx_;
                break;
                case QrPlacement::TopRight:
                qrX = panelRect.x + panelRect.w + qrMarginPx_;
                qrY = panelRect.y + qrMarginPx_;
                break;
                case QrPlacement::TopLeft:
                qrX = panelRect.x - qrMarginPx_ - q.w;
                qrY = panelRect.y + qrMarginPx_;
                break;
                case QrPlacement::BottomRight:
                qrX = panelRect.x + panelRect.w + qrMarginPx_;
                qrY = panelRect.y + panelRect.h - q.h - qrMarginPx_;
                break;
                case QrPlacement::BottomLeft:
                qrX = panelRect.x - qrMarginPx_ - q.w;
                qrY = panelRect.y + panelRect.h - q.h - qrMarginPx_;
                break;
                case QrPlacement::RightMiddle:
                qrX = panelRect.x + panelRect.w + qrMarginPx_;
                qrY = panelRect.y + (panelRect.h - q.h) * 0.5f;
                break;
                case QrPlacement::LeftMiddle:
                qrX = panelRect.x - qrMarginPx_ - q.w;
                qrY = panelRect.y + (panelRect.h - q.h) * 0.5f;
                break;
            }

            q.dst = { qrX, qrY, (float)q.w, (float)q.h };

            // Scale to composite RT coordinates
            const float layoutW = baseViewInfo.Width;
            const float layoutH = baseViewInfo.Height;
            const float targetW = (float)compW_;
            const float targetH = (float)compH_;

            if (layoutW > 0.f && layoutH > 0.f) {
                const float sx = targetW / layoutW;
                const float sy = targetH / layoutH;
                q.dst.x *= sx;
                q.dst.y *= sy;
                q.dst.w *= sx;
                q.dst.h *= sy;
            }
        }
    }

    SDL_SetRenderTarget(renderer, old);
    needsRedraw_ = false;
}