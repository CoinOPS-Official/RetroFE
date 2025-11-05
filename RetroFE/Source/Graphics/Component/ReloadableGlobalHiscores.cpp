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
	, tablesNeedRedraw_(true)  // Tables need initial rendering
    , needsRedraw_(true)
    , highScoreTable_(nullptr)
    , tableTexture_(nullptr)
    , intermediateTexture_(nullptr)
    , prevCompositeTexture_(nullptr)
    , crossfadeTexture_(nullptr)
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
    , qrMarginPx_(8) {
}

ReloadableGlobalHiscores::~ReloadableGlobalHiscores() {
    freeGraphicsMemory();
}

// ============================================================================
// State Helper Functions
// ============================================================================

void ReloadableGlobalHiscores::beginContext_(bool resetQr) {
    pagePhase_ = PagePhase::Single;
    pageT_ = 0.f;

    if (resetQr) {
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;

        // Clear QR texture cache when game changes
        for (SDL_Texture* tex : cachedQrTextures_) {
            if (tex) SDL_DestroyTexture(tex);
        }
        cachedQrTextures_.clear();
        cachedQrSizes_.clear();
        cachedQrGameIds_.clear();
    }

    gridPageIndex_ = 0;
    gridTimerSec_ = 0.f;
    gridBaselineValid_ = false;

    needsRedraw_ = true;
    tablesNeedRedraw_ = true;
    reloadDebounceTimer_ = reloadDebounceSec_;

    if (prevCompositeTexture_) {
        SDL_DestroyTexture(prevCompositeTexture_);
        prevCompositeTexture_ = nullptr;
    }

    // NEW: Clear layout cache
    cachedTableLayouts_.clear();
}

void ReloadableGlobalHiscores::beginPageFlip_() {
    pagePhase_ = PagePhase::SnapshotPending;
    pageT_ = 0.f;

    needsRedraw_ = true;
    tablesNeedRedraw_ = true;  // Different page = different tables
    reloadDebounceTimer_ = reloadDebounceSec_;
}

void ReloadableGlobalHiscores::snapshotPrevPage_(SDL_Renderer* r, int compositeW, int compositeH) {
    // If we already have a snapshot, don't recreate
    if (prevCompositeTexture_) return;

    // Safety check - need something to snapshot
    if (!intermediateTexture_) return;

    // Create texture to hold previous composite (tables + QRs together)
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

    // Copy current composite (which includes QRs) into prev
    SDL_Texture* oldRT = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, prevCompositeTexture_);
    SDL_RenderCopy(r, intermediateTexture_, nullptr, nullptr);
    SDL_SetRenderTarget(r, oldRT);

    // Transition from SnapshotPending → Crossfading
    if (pagePhase_ == PagePhase::SnapshotPending) {
        pagePhase_ = PagePhase::Crossfading;
        pageT_ = 0.f;
    }
}

void ReloadableGlobalHiscores::computeAlphas_(
    float baseA,
    float& prevCompA,
    float& newCompA) const {

    // Compute crossfade interpolation factor
    float t = (pageDurationSec_ > 0.f) ? (pageT_ / pageDurationSec_) : 1.f;
    t = std::clamp(t, 0.f, 1.f);

    if (pagePhase_ == PagePhase::Crossfading) {
        prevCompA = baseA * (1.f - t);
        newCompA = baseA * t;
    }
    else {
        // Single OR SnapshotPending → show current composite at full alpha
        prevCompA = 0.f;
        newCompA = baseA;
    }
}

// ============================================================================
// Resource Management
// ============================================================================

void ReloadableGlobalHiscores::allocateGraphicsMemory() {
    Component::allocateGraphicsMemory();

    // Clear all renderer-specific resources
    if (tableTexture_) {
        SDL_DestroyTexture(tableTexture_);
        tableTexture_ = nullptr;
    }
    if (intermediateTexture_) {
        SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = nullptr;
    }
    if (prevCompositeTexture_) {
        SDL_DestroyTexture(prevCompositeTexture_);
        prevCompositeTexture_ = nullptr;
	}
    if (crossfadeTexture_) {
        SDL_DestroyTexture(crossfadeTexture_);
        crossfadeTexture_ = nullptr;
	}
    // Clear QR texture cache (renderer changed)
    for (SDL_Texture* tex : cachedQrTextures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    cachedQrTextures_.clear();
    cachedQrSizes_.clear();
    cachedQrGameIds_.clear();

    // Force full redraw
    tablesNeedRedraw_ = true;
    needsRedraw_ = true;

    reloadTexture();
}

void ReloadableGlobalHiscores::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    // Free QR textures
    for (SDL_Texture* tex : cachedQrTextures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    cachedQrTextures_.clear();
    cachedQrSizes_.clear();
    cachedQrGameIds_.clear();

    if (tableTexture_) {
        SDL_DestroyTexture(tableTexture_);
        tableTexture_ = nullptr;
    }

    if (intermediateTexture_) {
        SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = nullptr;
    }

    if (prevCompositeTexture_) {
        SDL_DestroyTexture(prevCompositeTexture_);
        prevCompositeTexture_ = nullptr;
    }

    if (crossfadeTexture_) {
        SDL_DestroyTexture(crossfadeTexture_);
        crossfadeTexture_ = nullptr;
    }
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
        }
    }

    // Update QR state
    if (!visible) {
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;

        // Clean up composites when hidden
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }
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
                needsRedraw_ = true;  // Start QR rendering
            }
            break;

            case QrPhase::FadingIn:
            qrT_ += dt;
            needsRedraw_ = true;  // Re-render every frame for fade
            if (qrT_ >= qrFadeSec_) {
                qrPhase_ = QrPhase::Visible;
                qrT_ = qrFadeSec_;
                needsRedraw_ = true;  // Final render at full opacity
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

    Item const* selectedItem = page.getSelectedItem(displayOffset_);
    bool dataChanged = false;

    // pointer change or scroll-triggered reload
    const bool selChange =
        (selectedItem != lastSelectedItem_) ||
        (newScrollItemSelected && getMenuScrollReload());

    lastSelectedItem_ = selectedItem;

    // Track whether id string changed (so we re-split only when needed)
    bool idsChanged = false;

    if (selectedItem && !selectedItem->iscoredId.empty()) {
        if (cachedIscoredId_ != selectedItem->iscoredId) {
            cachedIds_.clear();
            Utils::listToVector(selectedItem->iscoredId, cachedIds_, ',');
            cachedIscoredId_ = selectedItem->iscoredId;

            lastSeenHashes_.clear();
            idsChanged = true;
        }

        // If the *item* changed but the string didn't, still clear hashes so we re-check
        if (selChange && !idsChanged) {
            lastSeenHashes_.clear();
        }

        // Compare content hashes
        for (const auto& id : cachedIds_) {
            const auto* gg = HiScores::getInstance().getGlobalGameById(id);
            if (!gg) continue;

            auto it = lastSeenHashes_.find(id);
            if (it == lastSeenHashes_.end() || it->second != gg->contentHash) {
                dataChanged = true;
                lastSeenHashes_[id] = gg->contentHash;
            }
        }

        // Prune any ids that are no longer present
        if (!lastSeenHashes_.empty()) {
            for (auto it = lastSeenHashes_.begin(); it != lastSeenHashes_.end(); ) {
                if (std::find(cachedIds_.begin(), cachedIds_.end(), it->first) == cachedIds_.end())
                    it = lastSeenHashes_.erase(it);
                else
                    ++it;
            }
        }
    }
    else {
        // No selection or empty ids → clear caches so nothing lingers
        if (!cachedIscoredId_.empty() || !cachedIds_.empty() || !lastSeenHashes_.empty()) {
            cachedIscoredId_.clear();
            cachedIds_.clear();
            lastSeenHashes_.clear();
            dataChanged = true; // context changed to "no data"
        }
    }

    // --- 4. Context change (new game/selection or data update) ---
    if (selChange || dataChanged) {
        beginContext_(selChange);  // This sets tablesNeedRedraw_ = true
        newItemSelected = false;
        newScrollItemSelected = false;

        if (geomChanged) {
            prevGeomW_ = widthNow;
            prevGeomH_ = heightNow;
            prevGeomFont_ = baseViewInfo.FontSize;
        }

        return Component::update(dt);
    }

    // --- 5. Geometry-only changes ---
    if (geomChanged) {
        prevGeomW_ = widthNow;
        prevGeomH_ = heightNow;
        prevGeomFont_ = baseViewInfo.FontSize;
        needsRedraw_ = true;
        tablesNeedRedraw_ = true;  // NEW: Geometry changed = re-render tables
        gridBaselineValid_ = false;
        reloadDebounceTimer_ = reloadDebounceSec_;
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
                beginPageFlip_();  // This sets tablesNeedRedraw_ = true
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

    // Rebuild texture if needed
    if (needsRedraw_ && reloadDebounceTimer_ <= 0.f) {
        reloadTexture();
    }

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (!renderer || !intermediateTexture_) return;

    SDL_FRect rect = {
        baseViewInfo.XRelativeToOrigin(),
        baseViewInfo.YRelativeToOrigin(),
        (float)compW_,
        (float)compH_
    };

    const float baseAlpha = baseViewInfo.Alpha;
    const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
    const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);

    // Compute alphas for page crossfade
    float prevCompA, newCompA;
    computeAlphas_(baseAlpha, prevCompA, newCompA);

    // === OPTIMIZATION: Pre-composite during crossfade ===
    if (pagePhase_ == PagePhase::Crossfading && prevCompositeTexture_) {
        // Create/resize crossfade texture if needed
        if (!crossfadeTexture_ || compW_ != (int)std::lround(baseViewInfo.ScaledWidth()) ||
            compH_ != (int)std::lround(baseViewInfo.ScaledHeight())) {

            if (crossfadeTexture_) {
                SDL_DestroyTexture(crossfadeTexture_);
            }

            crossfadeTexture_ = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_TARGET,
                compW_,
                compH_
            );

            if (crossfadeTexture_) {
                SDL_SetTextureBlendMode(crossfadeTexture_, SDL_BLENDMODE_BLEND);
            }
        }

        if (crossfadeTexture_) {
            // Render old + new to crossfade texture
            SDL_Texture* oldRT = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, crossfadeTexture_);

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

            // Compute relative alphas (baseAlpha will be applied by SDL::renderCopyF)
            const float relPrevA = (baseAlpha > 0.f) ? (prevCompA / baseAlpha) : 0.f;
            const float relNewA = (baseAlpha > 0.f) ? (newCompA / baseAlpha) : 1.f;

            // Composite old page
            if (relPrevA > 0.f) {
                SDL_SetTextureAlphaMod(prevCompositeTexture_, (Uint8)(relPrevA * 255.f));
                SDL_RenderCopy(renderer, prevCompositeTexture_, nullptr, nullptr);
            }

            // Composite new page
            if (relNewA > 0.f) {
                SDL_SetTextureAlphaMod(intermediateTexture_, (Uint8)(relNewA * 255.f));
                SDL_RenderCopy(renderer, intermediateTexture_, nullptr, nullptr);
            }

            SDL_SetRenderTarget(renderer, oldRT);

            // Single render of crossfaded result (expensive wrapper called ONCE)
            SDL::renderCopyF(crossfadeTexture_, baseAlpha,
                nullptr, &rect, baseViewInfo, layoutW, layoutH);

            return;  // Done!
        }
    }

    // === FALLBACK: Direct render (no crossfade or crossfade texture failed) ===

    // Render old page composite (if crossfading)
    if (prevCompA > 0.f && prevCompositeTexture_) {
        SDL::renderCopyF(prevCompositeTexture_, prevCompA,
            nullptr, &rect, baseViewInfo, layoutW, layoutH);
    }

    // Render new page composite
    if (newCompA > 0.f) {
        SDL::renderCopyF(intermediateTexture_, newCompA,
            nullptr, &rect, baseViewInfo, layoutW, layoutH);
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
    // --- Column alignment helper ---
    enum class ColAlign { Left, Center, Right };
    auto colAlignFor = [](size_t idx, size_t nCols) -> ColAlign {
        if (nCols >= 4) {
            if (idx == 0) return ColAlign::Left; // rank
            if (idx == 1) return ColAlign::Left; // name
            if (idx == 2) return ColAlign::Right; // score
            if (idx == 3) return ColAlign::Right; // time
        }
        return ColAlign::Center;
        };
    auto alignX = [](float x, float colW, float textW, ColAlign a) -> float {
        switch (a) {
            case ColAlign::Left: return x + 1.0f; // 1px guard
            case ColAlign::Center: return x + (colW - textW) * 0.5f;
            case ColAlign::Right: return x + (colW - textW);
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

        // Clear any lingering overlays / crossfade artifacts
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }

        // Reset QR state so nothing fades back in
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;

        // Clear the current composite texture to transparent
        if (intermediateTexture_ && renderer) {
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
        // No data for this game: purge any leftover overlays
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }

        // Keep QR system fully hidden
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;

        // Clear the current composite texture to transparent
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

    // Calculate composite dimensions (used for texture sizing and snapshot)
    const int compositeW = (int)std::lround(baseViewInfo.ScaledWidth());
    const int compositeH = (int)std::lround(baseViewInfo.ScaledHeight());

    // --- Create/resize intermediate texture BEFORE snapshot (needed for snapshot source) ---
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

    // --- Check if we need to reload QR textures ---
    bool qrCacheValid = (cachedQrGameIds_.size() == gameIds.size());
    if (qrCacheValid) {
        for (size_t i = 0; i < gameIds.size(); ++i) {
            if (i >= cachedQrGameIds_.size() || cachedQrGameIds_[i] != gameIds[i]) {
                qrCacheValid = false;
                break;
            }
        }
    }

    // --- Load/cache QR textures (ONLY when cache invalid) ---
    if (!qrCacheValid) {
        // Clear old cache
        for (SDL_Texture* tex : cachedQrTextures_) {
            if (tex) SDL_DestroyTexture(tex);
        }
        cachedQrTextures_.clear();
        cachedQrSizes_.clear();
        cachedQrGameIds_.clear();

        // Load new QRs as textures
        cachedQrTextures_.resize(gameIds.size(), nullptr);
        cachedQrSizes_.resize(gameIds.size(), { 0, 0 });

        for (size_t i = 0; i < gameIds.size(); ++i) {
            if (!gameIds[i].empty()) {
                std::string path = Configuration::absolutePath + "/iScored/qr/" + gameIds[i] + ".png";

                // Load surface temporarily
                SDL_Surface* surf = IMG_Load(path.c_str());
                if (surf) {
                    // Create texture from surface
                    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
                    if (tex) {
                        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
                        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                        cachedQrTextures_[i] = tex;
                        cachedQrSizes_[i] = { surf->w, surf->h };
                    }
                    // Free surface immediately (texture owns the data now)
                    SDL_FreeSurface(surf);
                }
            }
        }

        cachedQrGameIds_ = gameIds;
    }

    // --- Map cached textures to visible range ---
    std::vector<SDL_Texture*> qrTextures(Nvisible, nullptr);
    std::vector<std::pair<int, int>> qrSizes(Nvisible, { 0, 0 });

    for (int t = 0; t < Nvisible; ++t) {
        const int gi = startIdx + t;
        if (gi < (int)cachedQrTextures_.size()) {
            qrTextures[t] = cachedQrTextures_[gi];
            qrSizes[t] = cachedQrSizes_[gi];
        }
    }

    // --- QR reservation (size + placement) ---
    struct Margins { float L = 0, R = 0, T = 0, B = 0; };
    std::vector<Margins> qrReserve(Nvisible);
    const bool reserveVerticalForCorners = false;
    const bool reserveHorizontalForSides = true;

    for (int t = 0; t < Nvisible; ++t) {
        const auto& [qrW, qrH] = qrSizes[t];
        if (qrW == 0 || qrH == 0) continue;

        auto& m = qrReserve[t];

        switch (qrPlacement_) {
            case QrPlacement::TopCentered:
            m.T = (float)qrMarginPx_ + (float)qrH;
            break;
            case QrPlacement::BottomCenter:
            m.B = (float)qrMarginPx_ + (float)qrH;
            break;
            case QrPlacement::LeftMiddle:
            if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)qrW;
            break;
            case QrPlacement::RightMiddle:
            if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)qrW;
            break;
            case QrPlacement::TopLeft:
            if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)qrW;
            if (reserveVerticalForCorners) m.T = (float)qrMarginPx_ + (float)qrH;
            break;
            case QrPlacement::TopRight:
            if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)qrW;
            if (reserveVerticalForCorners) m.T = (float)qrMarginPx_ + (float)qrH;
            break;
            case QrPlacement::BottomLeft:
            if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)qrW;
            if (reserveVerticalForCorners) m.B = (float)qrMarginPx_ + (float)qrH;
            break;
            case QrPlacement::BottomRight:
            if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)qrW;
            if (reserveVerticalForCorners) m.B = (float)qrMarginPx_ + (float)qrH;
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

    // ========================================================================
    // STAGE 1: Render tables to tableTexture_ (ONLY when tables change)
    // ========================================================================

    if (tablesNeedRedraw_) {
        // NEW: Clear and resize layout cache
        cachedTableLayouts_.clear();
        cachedTableLayouts_.resize(Nvisible);

        // Create/resize table texture if needed
        if (!tableTexture_ || compW_ != compositeW || compH_ != compositeH) {
            if (tableTexture_) {
                SDL_DestroyTexture(tableTexture_);
            }
            tableTexture_ = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_TARGET,
                compositeW,
                compositeH
            );
            if (!tableTexture_) {
                needsRedraw_ = true;
                return;
            }
            SDL_SetTextureBlendMode(tableTexture_, SDL_BLENDMODE_BLEND);
        }

        SDL_Texture* old = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, tableTexture_);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        // Render all tables
        for (int t = 0; t < Nvisible; ++t) {
            const int gi = startIdx + t;
            const auto& table = highScoreTable_->tables[gi];

            const int slotCol = (t % cols);
            const int slotRow = (t / cols);

            const float xCell = std::round(slotCol * (cellW + spacingH));
            const float yCell = std::round(slotRow * (cellH + spacingV));

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
            const float panelH = titleH + headerH + dataH;
            const float panelW = totalWCols;

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

            // NEW: Store layout in cache before rendering
            auto& layout = cachedTableLayouts_[t];
            layout.finalScale = finalScale;
            layout.ratio = ratio;
            layout.panelW = panelW;
            layout.panelH = panelH;
            layout.anchorX = anchorX;
            layout.anchorY = anchorY;
            layout.drawX0 = drawX0;
            layout.titleH = titleH;
            layout.headerH = headerH;
            layout.dataH = dataH;
            layout.colW = colW;
            layout.panelRect = {
                drawX0 - kPanelGuardPx,
                anchorY + (float)extraT,
                panelW,
                panelH
            };

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
                    const bool ph = (r < (int)table.isPlaceholder.size() &&
                        c < table.isPlaceholder[r].size())
                        ? table.isPlaceholder[r][c]
                        : false;
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
        }

        SDL_SetRenderTarget(renderer, old);
        tablesNeedRedraw_ = false;  // Tables rendered, cache is fresh
    }

    // ========================================================================
    // STAGE 2: Composite tableTexture_ + QRs → intermediateTexture_
    // ========================================================================

    // intermediateTexture_ should already exist from above
    if (!intermediateTexture_) {
        needsRedraw_ = true;
        return;
    }

    SDL_Texture* old = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, intermediateTexture_);

    // Clear and copy cached tables
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    if (tableTexture_) {
        SDL_RenderCopy(renderer, tableTexture_, nullptr, nullptr);
    }

    // Add QRs on top (if QR phase allows)
    if (qrPhase_ == QrPhase::FadingIn || qrPhase_ == QrPhase::Visible) {
        // Calculate QR alpha
        float qrAlpha = 1.0f;
        if (qrPhase_ == QrPhase::FadingIn) {
            qrAlpha = std::clamp(qrT_ / qrFadeSec_, 0.f, 1.f);
        }

        // Composite QRs with current alpha
        for (int t = 0; t < Nvisible; ++t) {
            const auto& [qrW, qrH] = qrSizes[t];
            if (qrW == 0 || qrH == 0) continue;
            if (t >= (int)qrTextures.size() || !qrTextures[t]) continue;

            // NEW: Use cached layout (no recomputation)
            const auto& layout = cachedTableLayouts_[t];
            const SDL_FRect& panelRect = layout.panelRect;

            // Calculate QR position
            float qrX = 0.f, qrY = 0.f;
            switch (qrPlacement_) {
                case QrPlacement::TopCentered:
                qrX = panelRect.x + (panelRect.w - qrW) * 0.5f;
                qrY = panelRect.y - qrMarginPx_ - qrH;
                break;
                case QrPlacement::BottomCenter:
                qrX = panelRect.x + (panelRect.w - qrW) * 0.5f;
                qrY = panelRect.y + panelRect.h + qrMarginPx_;
                break;
                case QrPlacement::TopRight:
                qrX = panelRect.x + panelRect.w + qrMarginPx_;
                qrY = panelRect.y + qrMarginPx_;
                break;
                case QrPlacement::TopLeft:
                qrX = panelRect.x - qrMarginPx_ - qrW;
                qrY = panelRect.y + qrMarginPx_;
                break;
                case QrPlacement::BottomRight:
                qrX = panelRect.x + panelRect.w + qrMarginPx_;
                qrY = panelRect.y + panelRect.h - qrH - qrMarginPx_;
                break;
                case QrPlacement::BottomLeft:
                qrX = panelRect.x - qrMarginPx_ - qrW;
                qrY = panelRect.y + panelRect.h - qrH - qrMarginPx_;
                break;
                case QrPlacement::RightMiddle:
                qrX = panelRect.x + panelRect.w + qrMarginPx_;
                qrY = panelRect.y + (panelRect.h - qrH) * 0.5f;
                break;
                case QrPlacement::LeftMiddle:
                qrX = panelRect.x - qrMarginPx_ - qrW;
                qrY = panelRect.y + (panelRect.h - qrH) * 0.5f;
                break;
            }

            // Use cached texture - just modulate alpha!
            SDL_SetTextureAlphaMod(qrTextures[t], (Uint8)(qrAlpha * 255));

            SDL_FRect qrDst = { std::round(qrX), std::round(qrY), std::round((float)qrW), std::round((float)qrH) };
            SDL_RenderCopyF(renderer, qrTextures[t], nullptr, &qrDst);

            // NO destruction - texture is cached!
        }
    }

    SDL_SetRenderTarget(renderer, old);

    needsRedraw_ = false;
}