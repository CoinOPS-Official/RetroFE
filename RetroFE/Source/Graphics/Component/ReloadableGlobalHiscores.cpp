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

static inline void setAlphaIfChanged_(SDL_Texture* tex, Uint8& cache, Uint8 target) {
    if (tex && cache != target) {
        SDL_SetTextureAlphaMod(tex, target);
        cache = target;
    }
}

ReloadableGlobalHiscores::ReloadableGlobalHiscores(
    Configuration& /*config*/,
    std::string textFormat,
    Page& p,
    int displayOffset,
    FontManager* font,
    float baseColumnPadding,
    float baseRowPadding,
    std::string renderType,
    float qrDelaySec,
    float qrFadeSec)
    : Component(p)
    , fontInst_(font)
    , textFormat_(std::move(textFormat))
    , baseColumnPadding_(baseColumnPadding)
    , baseRowPadding_(baseRowPadding)
    , displayOffset_(displayOffset)
    , tablesNeedRedraw_(true)  // Tables need initial rendering
    , needsRedraw_(true)
    , highScoreTable_()
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
    , qrMarginPx_(8)
    , renderMode_(RenderMode::Both)
    , renderTable_(true)
    , renderQr_(true) {

    const std::string mode = Utils::toLower(Utils::trimEnds(renderType));
    if (mode == "qr") {
        renderMode_ = RenderMode::QrOnly;
        renderTable_ = false;
        renderQr_ = true;
    }
    else if (mode == "table") {
        renderMode_ = RenderMode::TableOnly;
        renderTable_ = true;
        renderQr_ = false;
    }

    if (std::isfinite(qrDelaySec) && qrDelaySec >= 0.0f) {
        qrDelaySec_ = qrDelaySec;
    }
    if (std::isfinite(qrFadeSec) && qrFadeSec >= 0.0f) {
        qrFadeSec_ = qrFadeSec;
    }
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

    // IMPORTANT:
    // When the selected item changes we may be in a debounce window before reloadTexture()
    // rebuilds the table set for the new item. During that window, update()'s page-rotation
    // logic must not compute a new grid baseline from the previous item's table set.
    // Nulling this pointer prevents stale baselines (e.g., multi-table -> single-table).
    highScoreTable_ = HighScoreData();

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
    SDL_SetTextureBlendMode(prevCompositeTexture_, SDL_BLENDMODE_BLEND);

    if (!prevCompositeTexture_) {
        Logger::write(Logger::ZONE_ERROR, "ReloadableGlobalHiscores",
            "Failed to create prevCompositeTexture_");
        return;
    }

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
    // Only rotate pages (and compute baselines) once the current selection has been rendered.
    // This avoids computing a grid baseline from a stale table set during the debounce window
    // after a selection change.
    if (!needsRedraw_ && reloadDebounceTimer_ <= 0.f && !highScoreTable_.tables.empty()) {
        const int totalTables = (int)highScoreTable_.tables.size();
        int pageSize = std::max(1, gridBaselineValid_ ? gridBaselineSlots_ : gridPageSize_);

        // If baseline isn't computed yet, compute it here so update() and reloadTexture() agree.
        if (!gridBaselineValid_) {
            FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
            if (font && baseViewInfo.Width > 1.f && baseViewInfo.Height > 1.f) {
                const float baseScale = baseViewInfo.FontSize / (float)font->getMaxHeight();
                const float asc = (float)font->getMaxAscent();
                computeGridBaseline_(font, totalTables, baseViewInfo.Width, baseViewInfo.Height, baseScale, asc);
                pageSize = std::max(1, gridBaselineValid_ ? gridBaselineSlots_ : gridPageSize_);
            }
        }

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

    if (pagePhase_ == PagePhase::Crossfading && prevCompositeTexture_) {
        // Ensure crossfadeTexture_ exists at the right size
        const int wantW = compW_;
        const int wantH = compH_;
        if (!crossfadeTexture_ || wantW <= 0 || wantH <= 0 ||
            wantW != (int)std::lround(baseViewInfo.ScaledWidth()) ||
            wantH != (int)std::lround(baseViewInfo.ScaledHeight())) {

            if (crossfadeTexture_) SDL_DestroyTexture(crossfadeTexture_);
            crossfadeTexture_ = SDL_CreateTexture(renderer,
                SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_TARGET,
                wantW, wantH);
            SDL_SetTextureBlendMode(crossfadeTexture_, SDL_BLENDMODE_BLEND);
            // Reset alpha caches upon (re)creation so first mod definitely sets
            prevPageAlphaCache_ = 255;
            newPageAlphaCache_ = 255;
        }

        if (crossfadeTexture_) {
            // Compute relative alphas once (your baseAlpha will be applied at the final renderCopyF)
            float prevCompA, newCompA;
            computeAlphas_(baseAlpha, prevCompA, newCompA);

            const float relPrevA = (baseAlpha > 0.f) ? (prevCompA / baseAlpha) : 0.f;
            const float relNewA = (baseAlpha > 0.f) ? (newCompA / baseAlpha) : 1.f;

            const Uint8 aPrev = (Uint8)std::lround(std::clamp(relPrevA, 0.f, 1.f) * 255.f);
            const Uint8 aNew = (Uint8)std::lround(std::clamp(relNewA, 0.f, 1.f) * 255.f);

            // Common early-outs
            if (aPrev == 0 && aNew == 0) {
                // Nothing to draw; just skip and fall back to nothing (rare)
                return;
            }
            if (aPrev == 0 && intermediateTexture_) {
                // Only new page visible → no need to clear: overwrite fully
                SDL_Texture* oldRT = SDL_GetRenderTarget(renderer);
                SDL_SetRenderTarget(renderer, crossfadeTexture_);
                setAlphaIfChanged_(intermediateTexture_, newPageAlphaCache_, aNew);
                SDL_RenderCopy(renderer, intermediateTexture_, nullptr, nullptr);
                SDL_SetRenderTarget(renderer, oldRT);

                SDL::renderCopyF(crossfadeTexture_, baseAlpha, nullptr, &rect, baseViewInfo, layoutW, layoutH);
                return;
            }
            if (aNew == 0 && prevCompositeTexture_) {
                // Only prev page visible → overwrite fully
                SDL_Texture* oldRT = SDL_GetRenderTarget(renderer);
                SDL_SetRenderTarget(renderer, crossfadeTexture_);
                setAlphaIfChanged_(prevCompositeTexture_, prevPageAlphaCache_, aPrev);
                SDL_RenderCopy(renderer, prevCompositeTexture_, nullptr, nullptr);
                SDL_SetRenderTarget(renderer, oldRT);

                SDL::renderCopyF(crossfadeTexture_, baseAlpha, nullptr, &rect, baseViewInfo, layoutW, layoutH);
                return;
            }

            // Both contribute → clear once, composite both layers with alpha state-change skip
            SDL_Texture* oldRT = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, crossfadeTexture_);

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

            setAlphaIfChanged_(prevCompositeTexture_, prevPageAlphaCache_, aPrev);
            SDL_RenderCopy(renderer, prevCompositeTexture_, nullptr, nullptr);

            setAlphaIfChanged_(intermediateTexture_, newPageAlphaCache_, aNew);
            SDL_RenderCopy(renderer, intermediateTexture_, nullptr, nullptr);

            SDL_SetRenderTarget(renderer, oldRT);

            // Single render of the result
            SDL::renderCopyF(crossfadeTexture_, baseAlpha, nullptr, &rect, baseViewInfo, layoutW, layoutH);
            return;
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
    gridBaselineValid_ = false;
    gridBaselineSlots_ = 0;
    gridBaselineCols_ = 0;
    gridBaselineRows_ = 0;
    gridBaselineCellW_ = 0.f;
    gridBaselineCellH_ = 0.f;
    gridBaselineRowMin_.clear();

    if (!font || highScoreTable_.tables.empty() || totalTables <= 0 || compW <= 1.f || compH <= 1.f) {
        return;
    }

    // --------------------------------------------------------------------
    // Minimum text size policy (the knob you tune)
    //
    // baseScale * font->getMaxHeight() is effectively baseViewInfo.FontSize in px.
    // We reject any grid layout where the *row-scale* (quantized-down) would
    // make text smaller than minTextPx.
    // --------------------------------------------------------------------
    constexpr float kMinTextPx = 28.0f; // <--- TUNE THIS (e.g. 14..22 depending on your theme)
    const float baseFontPx = baseScale * (float)font->getMaxHeight();
    float minScaleThreshold = (baseFontPx > 0.f) ? (kMinTextPx / baseFontPx) : 0.f;
    minScaleThreshold = std::clamp(minScaleThreshold, 0.f, 1.f);

    const int configuredPageSize = std::max(1, gridPageSize_);
    const int maxSlots = std::min(configuredPageSize, totalTables);

    const float spacingH = cellSpacingH_ * compW;
    const float spacingV = cellSpacingV_ * compH;

    auto ceilDiv = [](int a, int b) { return (a + b - 1) / b; };
    auto quantize_down = [](float s) {
        const float q = 64.f; // quantize to 1/64th (must match reloadTexture)
        return std::max(0.f, std::floor(s * q) / q);
        };

    struct Candidate {
        bool valid = false;
        int slots = 0;
        int cols = 0;
        int rows = 0;
        float cellW = 0.f;
        float cellH = 0.f;
        float minRowScale = 0.f;
        float shapeFit = -1e9f;
        std::vector<float> rowMin;
    };

    Candidate best;

    // Iterate slots from most-dense to least-dense; pick the first slots count
    // that can satisfy min text size, then choose the "best looking" columns.
    for (int slots = maxSlots; slots >= 1; --slots) {

        int colStart = 1;
        int colEnd = slots;

        if (gridColsHint_ > 0) {
            const int hinted = std::max(1, std::min(gridColsHint_, slots));
            colStart = hinted;
            colEnd = hinted;
        }

        Candidate bestForThisSlots;

        for (int cols = colStart; cols <= colEnd; ++cols) {
            const int rows = ceilDiv(slots, cols);

            // Compute cell geometry
            const float totalW = compW - spacingH * (cols - 1);
            const float totalH = compH - spacingV * (rows - 1);
            if (totalW <= 1.f || totalH <= 1.f) continue;

            const float cellW = totalW / (float)cols;
            const float cellH = totalH / (float)rows;
            if (cellW <= 1.f || cellH <= 1.f) continue;

            const int firstPageCount = std::min(slots, totalTables);

            std::vector<float> needScale(firstPageCount, 1.0f);
            std::vector<float> tableAspect(firstPageCount, 1.0f);

            // Estimate per-table fit scale using the same basic model used in reloadTexture
            const float drawableH0 = asc * baseScale;
            const float lineH0 = drawableH0 * (1.0f + baseRowPadding_);
            const float colPad0 = baseColumnPadding_ * drawableH0;

            for (int t = 0; t < firstPageCount; ++t) {
                const auto& table = highScoreTable_.tables[t];

                // Width: max per-column string width + padding between columns
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

                // Title can force width
                if (!table.id.empty()) {
                    width0 = std::max(width0, (float)font->getWidth(table.id) * baseScale);
                }

                // Height: title (optional) + header + kRowsPerPage rows
                float height0 = lineH0; // header
                if (!table.id.empty()) height0 += lineH0; // title
                height0 += lineH0 * kRowsPerPage;

                // Fit scale needed
                const float sW = (width0 > 0.f) ? (cellW / width0) : 1.0f;
                const float sH = (height0 > 0.f) ? (cellH / height0) : 1.0f;
                needScale[t] = std::min({ 1.0f, std::max(0.f, sW), std::max(0.f, sH) });

                if (height0 > 0.f && width0 > 0.f) {
                    tableAspect[t] = width0 / height0;
                }
            }

            // Row-wise min (and quantized down) so we never exceed fit
            std::vector<float> rowMin(rows, 1.0f);
            for (int r = 0; r < rows; ++r) {
                float s = 1.0f;
                for (int c = 0; c < cols; ++c) {
                    const int i = r * cols + c;
                    if (i >= firstPageCount) break;
                    s = std::min(s, needScale[i]);
                }
                rowMin[r] = quantize_down(s);
            }

            const float minRowScale = *std::min_element(rowMin.begin(), rowMin.end());

            // Enforce minimum text size
            if (minRowScale < minScaleThreshold) {
                continue;
            }

            // Prefer cell aspect close to typical table aspect (avoids 1xN strip layouts)
            float avgAspect = 1.0f;
            if (!tableAspect.empty()) {
                float sum = 0.f;
                for (float a : tableAspect) sum += a;
                avgAspect = sum / (float)tableAspect.size();
                if (!std::isfinite(avgAspect) || avgAspect <= 0.f) avgAspect = 1.0f;
            }

            const float cellAspect = (cellH > 0.f) ? (cellW / cellH) : 1.0f;
            const float eps = 1e-3f;
            const float shapeFit = -std::abs(std::log((cellAspect + eps) / (avgAspect + eps))); // closer to 0 is better

            // Keep the best-looking layout for this slots count
            if (!bestForThisSlots.valid ||
                (shapeFit > bestForThisSlots.shapeFit + 1e-6f) ||
                (std::abs(shapeFit - bestForThisSlots.shapeFit) <= 1e-6f && minRowScale > bestForThisSlots.minRowScale + 1e-6f)) {

                bestForThisSlots.valid = true;
                bestForThisSlots.slots = slots;
                bestForThisSlots.cols = cols;
                bestForThisSlots.rows = rows;
                bestForThisSlots.cellW = cellW;
                bestForThisSlots.cellH = cellH;
                bestForThisSlots.minRowScale = minRowScale;
                bestForThisSlots.shapeFit = shapeFit;
                bestForThisSlots.rowMin = std::move(rowMin);
            }
        }

        if (bestForThisSlots.valid) {
            best = std::move(bestForThisSlots);
            break; // slots are descending; first valid slots count wins
        }
    }

    if (!best.valid) {
        // Fallback: at least compute something sensible for a single slot.
        const int slots = std::min(1, totalTables);
        const int cols = 1;
        const int rows = 1;
        gridBaselineSlots_ = slots;
        gridBaselineCols_ = cols;
        gridBaselineRows_ = rows;
        gridBaselineCellW_ = compW;
        gridBaselineCellH_ = compH;
        gridBaselineRowMin_.assign(1, 1.f);
        gridBaselineValid_ = true;
        return;
    }

    gridBaselineSlots_ = best.slots;
    gridBaselineCols_ = best.cols;
    gridBaselineRows_ = best.rows;
    gridBaselineCellW_ = best.cellW;
    gridBaselineCellH_ = best.cellH;
    gridBaselineRowMin_ = std::move(best.rowMin);
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

            SDL_SetTextureColorMod(fillTex, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
            if (mip->dynamicFillTexture) {
                SDL_SetTextureColorMod(mip->dynamicFillTexture, baseViewInfo.textColor.r, baseViewInfo.textColor.g, baseViewInfo.textColor.b);
            }

            // Helper to process UTF-8 strings for both passes
            auto renderPass = [&](SDL_Texture* tex, bool isFill) {
                if (!tex) return;
                const unsigned char* ptr = (const unsigned char*)s.c_str();
                const unsigned char* end = ptr + s.length();
                float penX = x;
                Uint16 prev = 0;

                while (ptr < end) {
                    Uint32 ch;
                    // UTF-8 Decoding (1, 2, or 3 byte sequences)
                    if (*ptr < 0x80) ch = *ptr++;
                    else if ((*ptr & 0xE0) == 0xC0) {
                        if (ptr + 1 >= end) break;
                        ch = ((*ptr & 0x1F) << 6) | (*(ptr + 1) & 0x3F);
                        ptr += 2;
                    }
                    else if ((*ptr & 0xF0) == 0xE0) {
                        if (ptr + 2 >= end) break;
                        ch = ((*ptr & 0x0F) << 12) | ((*(ptr + 1) & 0x3F) << 6) | (*(ptr + 2) & 0x3F);
                        ptr += 3;
                    }
                    else { ptr++; continue; }

                    if (prev) penX += f->getKerning(prev, (Uint16)ch) * finalScale;

                    auto it = mip->glyphs.find((Uint16)ch);
                    if (it != mip->glyphs.end()) {
                        const auto& g = it->second;
                        if (isFill) {
                            SDL_Rect srcFill{ g.rect.x + g.fillX, g.rect.y + g.fillY, g.fillW, g.fillH };
                            SDL_FRect dstFill{ penX, ySnap, g.fillW * k, g.fillH * k };
                            SDL_RenderCopyF(r, tex, &srcFill, &dstFill);
                        }
                        else {
                            const SDL_Rect& src = g.rect;
                            SDL_FRect dst = { penX - g.fillX * k, ySnap - g.fillY * k, src.w * k, src.h * k };
                            SDL_RenderCopyF(r, tex, &src, &dst);
                        }
                        penX += g.advance * k;
                    }
                    prev = (Uint16)ch;
                }
                };

            renderPass(outlineTex, false);
            renderPass(fillTex, true);
        };

    // --- Exact width measure (mip + kerning + outline overhang) ---
    auto measureTextWidthExact = [&](FontManager* f, const std::string& s, float scale) -> float {
        if (!f || s.empty()) return 0.0f;
        const float targetH = scale * f->getMaxHeight();
        const FontManager::MipLevel* mip = f->getMipLevelForSize((int)targetH);
        if (!mip || !mip->fillTexture) return (float)f->getWidth(s) * scale;

        const float k = (mip->height > 0) ? (targetH / mip->height) : 1.0f;
        const bool hasOutline = (mip->outlineTexture != nullptr);

        const unsigned char* ptr = (const unsigned char*)s.c_str();
        const unsigned char* end = ptr + s.length();
        float penX = 0.0f, minX = 0.0f, maxX = 0.0f;
        bool first = true;
        Uint16 prev = 0;

        while (ptr < end) {
            Uint32 ch;
            if (*ptr < 0x80) ch = *ptr++;
            else if ((*ptr & 0xE0) == 0xC0) {
                if (ptr + 1 >= end) break;
                ch = ((*ptr & 0x1F) << 6) | (*(ptr + 1) & 0x3F);
                ptr += 2;
            }
            else if ((*ptr & 0xF0) == 0xE0) {
                if (ptr + 2 >= end) break;
                ch = ((*ptr & 0x0F) << 12) | ((*(ptr + 1) & 0x3F) << 6) | (*(ptr + 2) & 0x3F);
                ptr += 3;
            }
            else { ptr++; continue; }

            if (prev) penX += f->getKerning(prev, (Uint16)ch) * scale;

            auto it = mip->glyphs.find((Uint16)ch);
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
            prev = (Uint16)ch;
        }
        return std::max(0.0f, maxX - minX);
        };

    // --- Column alignment helper ---
    enum class ColAlign { Left, Center, Right };
    auto colAlignFor = [](size_t idx, size_t nCols) -> ColAlign {
        if (nCols >= 4) {
            if (idx == 0) return ColAlign::Left;  // rank
            if (idx == 1) return ColAlign::Left;  // name
            if (idx == 2) return ColAlign::Right; // score
            if (idx == 3) return ColAlign::Right; // time
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
    auto clampf = [](float v, float lo, float hi) {
        return std::max(lo, std::min(v, hi));
        };
    auto quantize_down = [](float s) {
        const float q = 64.f; // quantize to 1/64th
        return std::max(0.f, std::floor(s * q) / q);
        };

    // Small helper: compute/snap a QR dst given a panel rect and QR size
    auto placeQrInPanel = [&](const SDL_FRect& panel, int qrW, int qrH) -> SDL_FRect {
        SDL_FRect r{};
        float x = 0.f, y = 0.f;

        switch (qrPlacement_) {
            case QrPlacement::TopCentered:
            x = panel.x + (panel.w - qrW) * 0.5f;
            y = panel.y - qrMarginPx_ - qrH;
            break;
            case QrPlacement::BottomCenter:
            x = panel.x + (panel.w - qrW) * 0.5f;
            y = panel.y + panel.h + qrMarginPx_;
            break;
            case QrPlacement::TopRight:
            x = panel.x + panel.w + qrMarginPx_;
            y = panel.y + qrMarginPx_;
            break;
            case QrPlacement::TopLeft:
            x = panel.x - qrMarginPx_ - qrW;
            y = panel.y + qrMarginPx_;
            break;
            case QrPlacement::BottomRight:
            x = panel.x + panel.w + qrMarginPx_;
            y = panel.y + panel.h - qrH - qrMarginPx_;
            break;
            case QrPlacement::BottomLeft:
            x = panel.x - qrMarginPx_ - qrW;
            y = panel.y + panel.h - qrH - qrMarginPx_;
            break;
            case QrPlacement::RightMiddle:
            x = panel.x + panel.w + qrMarginPx_;
            y = panel.y + (panel.h - qrH) * 0.5f;
            break;
            case QrPlacement::LeftMiddle:
            x = panel.x - qrMarginPx_ - qrW;
            y = panel.y + (panel.h - qrH) * 0.5f;
            break;
        }

        r.x = std::round(x);
        r.y = std::round(y);
        r.w = std::round((float)qrW);
        r.h = std::round((float)qrH);
        return r;
        };

    // --- Setup / early-outs ---
    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);

    Item* selectedItem = page.getSelectedItem(displayOffset_);
    if (!selectedItem || !renderer) {
        highScoreTable_ = HighScoreData();

        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }

        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;

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
    if (highScoreTable_.tables.empty()) {
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }

        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;

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

    const int totalTables = (int)highScoreTable_.tables.size();
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

    // Composite (render target) size
    const int compositeW = (int)std::lround(baseViewInfo.ScaledWidth());
    const int compositeH = (int)std::lround(baseViewInfo.ScaledHeight());

    // Create/resize the intermediate composite (set blend ONCE at creation)
    if (!intermediateTexture_ || compW_ != compositeW || compH_ != compositeH) {
        if (intermediateTexture_) SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_TARGET,
            compositeW, compositeH);
        if (!intermediateTexture_) { needsRedraw_ = true; return; }
        SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND);
        compW_ = compositeW;
        compH_ = compositeH;

        // Resized → we’ll need fresh layout + QR dsts
        tablesNeedRedraw_ = true;
        for (auto& L : cachedTableLayouts_) L.qrDstDirty = true;
    }

    // Snapshot previous page if we’re entering a crossfade
    if (pagePhase_ == PagePhase::SnapshotPending) {
        snapshotPrevPage_(renderer, compositeW, compositeH);
    }
    else if (pagePhase_ == PagePhase::Single) {
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }
    }

    // Compute grid baseline if needed
    if (!gridBaselineValid_) {
        computeGridBaseline_(font, totalTables, compW, compH, baseScale, asc);
    }
    const int cols = gridBaselineCols_;
    const int rows = gridBaselineRows_;
    const float cellW = gridBaselineCellW_;
    const float cellH = gridBaselineCellH_;
    const float spacingH = cellSpacingH_ * compW;
    const float spacingV = cellSpacingV_ * compH;

    const int pageSize = std::max(1, gridBaselineValid_ ? gridBaselineSlots_ : gridPageSize_);

    const auto [startIdx, Nvisible] =
        computeVisibleRange(totalTables, gridPageIndex_, pageSize);
    if (Nvisible <= 0) {
        needsRedraw_ = true;
        return;
    }

    std::vector<SDL_Texture*> qrTextures(Nvisible, nullptr);
    std::vector<std::pair<int, int>> qrSizes(Nvisible, { 0,0 });

    // --- Resolve the list of QR IDs for the selected game ---
    if (renderQr_) {
        std::vector<std::string> gameIds;
        if (!selectedItem->iscoredId.empty()) {
            Utils::listToVector(selectedItem->iscoredId, gameIds, ',');
        }

        // Check QR cache validity by IDs
        bool qrCacheValid = (cachedQrGameIds_.size() == gameIds.size());
        if (qrCacheValid) {
            for (size_t i = 0; i < gameIds.size(); ++i) {
                if (i >= cachedQrGameIds_.size() || cachedQrGameIds_[i] != gameIds[i]) {
                    qrCacheValid = false; break;
                }
            }
        }

        // (Re)load QR textures only if IDs changed
        if (!qrCacheValid) {
            for (SDL_Texture* tex : cachedQrTextures_) if (tex) SDL_DestroyTexture(tex);
            cachedQrTextures_.clear();
            cachedQrSizes_.clear();
            cachedQrGameIds_.clear();

            cachedQrTextures_.resize(gameIds.size(), nullptr);
            cachedQrSizes_.resize(gameIds.size(), { 0,0 });

            for (size_t i = 0; i < gameIds.size(); ++i) {
                if (gameIds[i].empty()) continue;
                const std::string path = Configuration::absolutePath + "/iScored/qr/" + gameIds[i] + ".png";
                if (SDL_Surface* surf = IMG_Load(path.c_str())) {
                    if (SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf)) {
                        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
                        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND); // set once
                        cachedQrTextures_[i] = tex;
                        cachedQrSizes_[i] = { surf->w, surf->h };
                    }
                    SDL_FreeSurface(surf);
                }
            }

            cachedQrGameIds_ = gameIds;
            // IDs changed → sizes likely changed somewhere; mark all dirty
            for (auto& L : cachedTableLayouts_) L.qrDstDirty = true;
        }

        // Map cached QR textures/sizes to the visible range
        for (int t = 0; t < Nvisible; ++t) {
            const int gi = startIdx + t;
            if (gi < (int)cachedQrTextures_.size()) {
                qrTextures[t] = cachedQrTextures_[gi];
                qrSizes[t] = cachedQrSizes_[gi];
            }
        }
    }

    // Compute QR “reserve” margins (affects table panel layout)
    struct Margins { float L = 0, R = 0, T = 0, B = 0; };
    std::vector<Margins> qrReserve(Nvisible);
    const bool reserveVerticalForCorners = false;
    const bool reserveHorizontalForSides = true;
    for (int t = 0; t < Nvisible; ++t) {
        const auto& [qrW, qrH] = qrSizes[t];
        if (qrW == 0 || qrH == 0) continue;
        auto& m = qrReserve[t];
        switch (qrPlacement_) {
            case QrPlacement::TopCentered:    m.T = (float)qrMarginPx_ + (float)qrH;
                break;
            case QrPlacement::BottomCenter:   m.B = (float)qrMarginPx_ + (float)qrH;
                break;
            case QrPlacement::LeftMiddle:     if (reserveHorizontalForSides) m.L = (float)qrMarginPx_ + (float)qrW;
                break;
            case QrPlacement::RightMiddle:    if (reserveHorizontalForSides) m.R = (float)qrMarginPx_ + (float)qrW;
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
        }
    }
    std::vector<float> qrExtraW(Nvisible, 0.f), qrExtraH(Nvisible, 0.f);
    for (int t = 0; t < Nvisible; ++t) {
        qrExtraW[t] = qrReserve[t].L + qrReserve[t].R;
        qrExtraH[t] = qrReserve[t].T + qrReserve[t].B;
    }

    // Shared column layout (measure exact widths at baseScale)
    //
    // IMPORTANT:
    // We intentionally compute shared column widths across *all* tables for the
    // current game (not just the visible page). Otherwise, the widest title/value
    // might be on page 0, causing page 0 to scale down (leaving vertical slack),
    // while page 1 scales up (appearing to "fix itself" when the page flips).
    // This keeps the layout stable across page rotation.
    size_t maxCols = 0;
    for (int gi = 0; gi < totalTables; ++gi)
        maxCols = std::max(maxCols, highScoreTable_.tables[gi].columns.size());
    if (maxCols == 0) { needsRedraw_ = true; return; }

    std::vector<float> maxColW0(maxCols, 0.f);
    float maxTitleW0 = 0.f;
    float height0Max = 0.f; // use worst-case height so scale doesn't vary per-page

    for (int gi = 0; gi < totalTables; ++gi) {
        const auto& table = highScoreTable_.tables[gi];

        for (size_t c = 0; c < maxCols; ++c) {
            float w = 0.f;
            if (c < table.columns.size()) {
                w = measureTextWidthExact(font, table.columns[c], baseScale);
                for (const auto& row : table.rows) {
                    if (c < row.size())
                        w = std::max(w, measureTextWidthExact(font, row[c], baseScale));
                }
            }
            maxColW0[c] = std::max(maxColW0[c], w);
        }

        if (!table.id.empty())
            maxTitleW0 = std::max(maxTitleW0, measureTextWidthExact(font, table.id, baseScale));

        float h = lineH0;              // header
        if (!table.id.empty()) h += lineH0;  // title
        h += lineH0 * kRowsPerPage;    // rows
        height0Max = std::max(height0Max, h);
    }

    // Per-table heights for the visible set (used for panel sizing). We still
    // keep a max for fit-scale stability.
    std::vector<float> height0(Nvisible, std::max(height0Max, lineH0 * (1 + kRowsPerPage)));
    for (int t = 0; t < Nvisible; ++t) {
        const auto& table = highScoreTable_.tables[startIdx + t];
        float h = lineH0;
        if (!table.id.empty()) h += lineH0;
        h += lineH0 * kRowsPerPage;
        height0[t] = h;
    }

    // Build exact shared width @ baseScale (columns + pads + guard)
    float sumCols0 = 0.f;
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

    // Fit per slot (width/height)
    std::vector<float> needScale(Nvisible, 1.f);
    constexpr float fitEps = 0.5f;
    for (int t = 0; t < Nvisible; ++t) {
        const float availW = std::max(0.0f, cellW - qrExtraW[t] - fitEps);
        const float availH = std::max(0.0f, cellH - qrExtraH[t] - fitEps);

        float sW = sharedW0_exact > 0 ? (availW / sharedW0_exact) : 1.0f;
        // Use worst-case height for fitting so scale doesn't change per-page
        const float h0 = std::max(height0Max, height0[t]);
        float sH = h0 > 0 ? (availH / h0) : 1.0f;
        needScale[t] = std::min(std::min(1.0f, std::max(0.0f, sW)),
            std::min(1.0f, std::max(0.0f, sH)));
    }

    // Row-wise min, quantized down so we never exceed fit
    std::vector<float> rowScale(rows, 1.f);
    for (int r = 0; r < rows; ++r) {
        float s = 1.f;
        for (int c = 0; c < cols; ++c) {
            const int i = r * cols + c;
            if (i >= Nvisible) break;
            s = std::min(s, needScale[i]);
        }
        rowScale[r] = quantize_down(s);
    }

    // ========================================================================
    // STAGE 1: Render tables → tableTexture_ (ONLY when tables change)
    // ========================================================================
    if (tablesNeedRedraw_) {
        cachedTableLayouts_.assign(Nvisible, TableLayout{}); // reset & size exactly

        if (!tableTexture_ || compW_ != compositeW || compH_ != compositeH) {
            if (tableTexture_) SDL_DestroyTexture(tableTexture_);
            tableTexture_ = SDL_CreateTexture(renderer,
                SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_TARGET,
                compositeW, compositeH);
            if (!tableTexture_) { needsRedraw_ = true; return; }
            SDL_SetTextureBlendMode(tableTexture_, SDL_BLENDMODE_BLEND);
        }

        SDL_Texture* old = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, tableTexture_);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        for (int t = 0; t < Nvisible; ++t) {
            const int gi = startIdx + t;
            const auto& table = highScoreTable_.tables[gi];

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
            std::vector<float> colW(maxCols, 0.f);
            float totalWCols = 0.f;
            for (size_t c = 0; c < maxCols; ++c) {
                colW[c] = maxColW0[c] * ratio;
                totalWCols += colW[c];
                if (c + 1 < maxCols) totalWCols += colPad;
            }
            totalWCols += 2.0f * kPanelGuardPx;

            // Heights
            const float titleH = table.id.empty() ? 0.f : lineH;
            const float headerH = lineH;
            const float dataH = lineH * kRowsPerPage;
            const float panelH = titleH + headerH + dataH;
            const float panelW = totalWCols;

            // QR margins (same as in fit)
            const int extraL = (int)std::lround(qrReserve[t].L);
            const int extraR = (int)std::lround(qrReserve[t].R);
            const int extraT = (int)std::lround(qrReserve[t].T);
            const int extraB = (int)std::lround(qrReserve[t].B);

            // Anchor inside cell
            float anchorW = panelW + (float)(extraL + extraR);
            float anchorH = panelH + (float)(extraT + extraB);

            float anchorX = xCell + (cellW - anchorW) * 0.5f;
            // Center vertically too. If scale is width-limited, this prevents the
            // table from looking "stuck to the top" with a big empty area below.
            float anchorY = yCell + (cellH - anchorH) * 0.5f;
            anchorX = std::round(clampf(anchorX, xCell, xCell + cellW - anchorW));
            anchorY = std::round(clampf(anchorY, yCell, yCell + cellH - anchorH));

            // Text origin
            float drawX0 = anchorX + (float)extraL + kPanelGuardPx;
            float y = anchorY + (float)extraT;

            // Store panel in cache (and mark QR dst dirty because panel changed)
            auto& layout = cachedTableLayouts_[t];
            layout.panelRect = {
                drawX0 - kPanelGuardPx,
                anchorY + (float)extraT,
                panelW,
                panelH
            };
            layout.qrDstDirty = true; // panel changed → must re-place QR

            // ---- Title ----
            if (!table.id.empty()) {
                float w = measureTextWidthExact(font, table.id, finalScale);
                float x = std::round((totalWCols - w) * 0.5f);
                renderTextOutlined(renderer, font, table.id, drawX0 + x, y, finalScale);
                y += lineH;
            }

            // ---- Headers ----
            {
                float x = 0.f;
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
                float x = 0.f;
                const auto* rowV = (r < (int)table.rows.size()) ? &table.rows[r] : nullptr;
                for (size_t c = 0; c < maxCols; ++c) {
                    std::string cell;
                    if (rowV && c < rowV->size()) cell = (*rowV)[c];
                    const float tw = measureTextWidthExact(font, cell, finalScale);
                    const bool ph = (r < (int)table.isPlaceholder.size() &&
                        c < table.isPlaceholder[r].size())
                        ? table.isPlaceholder[r][c] : false;
                    const ColAlign a = ph ? ColAlign::Center : colAlignFor(c, maxCols);
                    const float xAligned = alignX(drawX0 + x, colW[c], tw, a);
                    if (!cell.empty())
                        renderTextOutlined(renderer, font, cell, std::round(xAligned), y, finalScale);
                    x += colW[c];
                    if (c + 1 < maxCols) x += colPad;
                }
                y += lineH;
            }
        }

        SDL_SetRenderTarget(renderer, old);
        tablesNeedRedraw_ = false;
        needsRedraw_ = true;
    }

    // ========================================================================
    // STAGE 1.5: Update per-table QR meta + recompute cached dst if dirty
    // ========================================================================
    // Ensure layout cache sized
    if ((int)cachedTableLayouts_.size() < Nvisible)
        cachedTableLayouts_.resize(Nvisible);

    for (int t = 0; t < Nvisible; ++t) {
        auto& L = cachedTableLayouts_[t];
        SDL_Texture* tex = qrTextures[t];
        int qW = qrSizes[t].first, qH = qrSizes[t].second;

        // Trust actual texture size if present
        if (tex && (qW == 0 || qH == 0)) {
            SDL_QueryTexture(tex, nullptr, nullptr, &qW, &qH);
            qrSizes[t] = { qW, qH };
        }

        const bool had = L.qrHave;
        L.qrHave = (tex != nullptr && qW > 0 && qH > 0);
        if (L.qrHave) {
            if (!had || L.qrSrcW != qW || L.qrSrcH != qH) {
                L.qrSrcW = qW; L.qrSrcH = qH;
                L.qrDstDirty = true; // size change → re-place
            }
            if (L.qrDstDirty) {
                if (renderMode_ == RenderMode::QrOnly) {
                    L.qrDst = {
                        0.0f,
                        0.0f,
                        static_cast<float>(compositeW),
                        static_cast<float>(compositeH)
                    };
                }
                else {
                    L.qrDst = placeQrInPanel(L.panelRect, L.qrSrcW, L.qrSrcH);
                }
                L.qrDstDirty = false;
            }
        }
        else {
            L.qrSrcW = L.qrSrcH = 0;
        }
    }

    // ========================================================================
    // STAGE 2: Composite tables + cached QR dsts → intermediateTexture_
    // ========================================================================
    if (!intermediateTexture_) { needsRedraw_ = true; return; }

    SDL_Texture* old = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, intermediateTexture_);

    // Clear and copy cached tables
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    if (renderTable_ && tableTexture_) {
        SDL_RenderCopy(renderer, tableTexture_, nullptr, nullptr);
    }

    // Add QRs on top (if QR phase allows)
    if (renderQr_ && (qrPhase_ == QrPhase::FadingIn || qrPhase_ == QrPhase::Visible)) {
        float qrAlphaF = 1.0f;
        if (qrPhase_ == QrPhase::FadingIn && qrFadeSec_ > 0.f) {
            qrAlphaF = std::clamp(qrT_ / qrFadeSec_, 0.f, 1.f);
        }
        const Uint8 qrAlpha = (Uint8)std::lround(qrAlphaF * 255.f);

        for (int t = 0; t < Nvisible; ++t) {
            SDL_Texture* tex = qrTextures[t];
            if (!tex) continue;

            const auto& L = cachedTableLayouts_[t];
            // QR dst rect is cached; no per-frame math
            SDL_SetTextureAlphaMod(tex, qrAlpha); // (optional) add per-QR alpha cache to skip repeats
            SDL_RenderCopyF(renderer, tex, nullptr, &L.qrDst);
        }
    }

    SDL_SetRenderTarget(renderer, old);
    needsRedraw_ = false;

    // If we just built the current page and need to enter a flip, snapshot now
    if (pagePhase_ == PagePhase::SnapshotPending) {
        snapshotPrevPage_(renderer, compositeW, compositeH);
    }
}
