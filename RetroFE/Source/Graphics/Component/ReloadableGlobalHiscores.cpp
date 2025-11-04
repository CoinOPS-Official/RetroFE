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
#include <filesystem>

namespace fs = std::filesystem;

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
    , qrMarginPx_(8) 
    , qrAtlasTextures_()              // ← Vector (empty initialization)
    , currentAtlasIndex_(0)            // ← NEW: Track which atlas we're filling
    , qrAtlasMap_()
    , qrAtlasWidth_(0)
    , qrAtlasHeight_(0)
    , qrAtlasMaxSize_(4096)
    , qrNextX_(2)
    , qrNextY_(2)
    , qrAtlasBuilt_(false) {
}

ReloadableGlobalHiscores::~ReloadableGlobalHiscores() {
    freeGraphicsMemory();
}

// ============================================================================
// State Helper Functions
// ============================================================================

void ReloadableGlobalHiscores::clearQrAtlas_() {
    for (SDL_Texture* atlas : qrAtlasTextures_) {
        if (atlas) {
            SDL_DestroyTexture(atlas);
        }
    }
    qrAtlasTextures_.clear();
    qrAtlasMap_.clear();
    currentAtlasIndex_ = 0;
    qrNextX_ = 2;
    qrNextY_ = 2;
    qrAtlasBuilt_ = false;
}

void ReloadableGlobalHiscores::buildInitialAtlas_(SDL_Renderer* renderer) {
    if (qrAtlasBuilt_) return;

    // Lock to 4096 max for broad GPU compatibility
    qrAtlasMaxSize_ = 4096;

    LOG_INFO("ReloadableGlobalHiscores",
        "Building QR atlas (max size: " + std::to_string(qrAtlasMaxSize_) + "×" +
        std::to_string(qrAtlasMaxSize_) + ")");

    const fs::path qrDir = fs::path(Configuration::absolutePath) / "iScored" / "qr";
    std::vector<std::string> qrFiles;

    try {
        if (fs::exists(qrDir) && fs::is_directory(qrDir)) {
            for (const auto& entry : fs::directory_iterator(qrDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".png") {
                    std::string stem = entry.path().stem().string();
                    if (stem == "qrmask") continue;
                    qrFiles.push_back(stem);
                }
            }
        }
    }
    catch (const fs::filesystem_error& e) {
        LOG_ERROR("ReloadableGlobalHiscores",
            "Failed to scan QR directory: " + std::string(e.what()));
    }

    constexpr int qrSize = 58;
    constexpr int padding = 2;
    constexpr int cellSize = qrSize + padding;

    // Small headroom: add space for ~50 new QRs per atlas
    const int qrCount = std::max(1, (int)qrFiles.size());
    const int qrCountWithHeadroom = qrCount + 50;

    const int qrPerRow = qrAtlasMaxSize_ / cellSize;  // 68 QRs per row
    const int rowsNeeded = (qrCountWithHeadroom + qrPerRow - 1) / qrPerRow;

    // Width: always full width for consistent packing
    qrAtlasWidth_ = qrAtlasMaxSize_;

    // Height: exact rows needed (no wasted space)
    qrAtlasHeight_ = std::min(rowsNeeded * cellSize, qrAtlasMaxSize_);

    const int capacityPerAtlas = (qrAtlasWidth_ / cellSize) * (qrAtlasHeight_ / cellSize);

    LOG_INFO("ReloadableGlobalHiscores",
        "Creating initial QR atlas: " + std::to_string(qrAtlasWidth_) + "×" +
        std::to_string(qrAtlasHeight_) +
        " for " + std::to_string(qrCount) + " QRs " +
        "(capacity: " + std::to_string(capacityPerAtlas) + ", " +
        std::to_string((int)((qrAtlasWidth_ * qrAtlasHeight_ * 4) / 1024 / 1024)) + " MB)");

    SDL_Texture* firstAtlas = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        qrAtlasWidth_,
        qrAtlasHeight_
    );

    if (!firstAtlas) {
        LOG_ERROR("ReloadableGlobalHiscores", "Failed to create QR atlas texture");
        return;
    }

    SDL_SetTextureBlendMode(firstAtlas, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(firstAtlas, SDL_ScaleModeNearest);

    void* pixels;
    int pitch;
    if (SDL_LockTexture(firstAtlas, nullptr, &pixels, &pitch) == 0) {
        memset(pixels, 0, pitch * qrAtlasHeight_);
        SDL_UnlockTexture(firstAtlas);
    }

    qrAtlasTextures_.push_back(firstAtlas);
    currentAtlasIndex_ = 0;
    qrAtlasBuilt_ = true;

    if (qrFiles.empty()) {
        LOG_INFO("ReloadableGlobalHiscores", "QR atlas ready (no QRs found)");
        return;
    }

    // Load all existing QRs
    int loadedCount = 0;
    int x = padding, y = padding;

    for (const auto& gameId : qrFiles) {
        fs::path qrPath = qrDir / (gameId + ".png");
        SDL_Surface* qrSurf = IMG_Load(qrPath.string().c_str());
        if (!qrSurf) continue;

        if (x + qrSize > qrAtlasWidth_) {
            x = padding;
            y += cellSize;
        }

        // Need new atlas?
        if (y + qrSize > qrAtlasHeight_) {
            LOG_INFO("ReloadableGlobalHiscores",
                "Atlas " + std::to_string(currentAtlasIndex_) + " full (" +
                std::to_string(loadedCount) + " QRs), creating overflow atlas");

            // Create new atlas
            SDL_Texture* newAtlas = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_STREAMING,
                qrAtlasMaxSize_,
                qrAtlasMaxSize_  // Full size for overflow
            );

            if (!newAtlas) {
                LOG_ERROR("ReloadableGlobalHiscores", "Failed to create overflow atlas");
                SDL_FreeSurface(qrSurf);
                break;
            }

            SDL_SetTextureBlendMode(newAtlas, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(newAtlas, SDL_ScaleModeNearest);

            if (SDL_LockTexture(newAtlas, nullptr, &pixels, &pitch) == 0) {
                memset(pixels, 0, pitch * qrAtlasMaxSize_);
                SDL_UnlockTexture(newAtlas);
            }

            qrAtlasTextures_.push_back(newAtlas);
            currentAtlasIndex_++;
            qrAtlasHeight_ = qrAtlasMaxSize_;  // Update to full size

            x = padding;
            y = padding;

            LOG_INFO("ReloadableGlobalHiscores",
                "Created overflow atlas " + std::to_string(currentAtlasIndex_) +
                " (" + std::to_string(qrAtlasMaxSize_) + "×" + std::to_string(qrAtlasMaxSize_) + ")");
        }

        SDL_Rect dstRect = { x, y, qrSize, qrSize };
        SDL_UpdateTexture(qrAtlasTextures_[currentAtlasIndex_], &dstRect, qrSurf->pixels, qrSurf->pitch);

        qrAtlasMap_[gameId] = { dstRect, gameId, currentAtlasIndex_ };  // Store atlas index

        SDL_FreeSurface(qrSurf);
        loadedCount++;
        x += cellSize;
    }

    qrNextX_ = x;
    qrNextY_ = y;

    LOG_INFO("ReloadableGlobalHiscores",
        "QR atlases loaded: " + std::to_string(qrAtlasTextures_.size()) + " atlas(es), " +
        std::to_string(loadedCount) + " QRs total");
}

bool ReloadableGlobalHiscores::addQrToAtlas_(SDL_Renderer* renderer, const std::string& gameId) {
    if (!qrAtlasBuilt_) {
        buildInitialAtlas_(renderer);
        if (!qrAtlasBuilt_) return false;
    }

    if (qrAtlasMap_.find(gameId) != qrAtlasMap_.end()) {
        return true;  // Already exists
    }

    const fs::path qrPath = fs::path(Configuration::absolutePath) / "iScored" / "qr" / (gameId + ".png");

    if (!fs::exists(qrPath)) {
        return false;
    }

    SDL_Surface* qrSurf = IMG_Load(qrPath.string().c_str());
    if (!qrSurf) return false;

    constexpr int qrSize = 58;
    constexpr int padding = 2;
    constexpr int cellSize = qrSize + padding;

    if (qrNextX_ + qrSize > qrAtlasWidth_) {
        qrNextX_ = padding;
        qrNextY_ += cellSize;
    }

    // Need new atlas?
    if (qrNextY_ + qrSize > qrAtlasHeight_) {
        LOG_INFO("ReloadableGlobalHiscores",
            "Current atlas " + std::to_string(currentAtlasIndex_) + " full, creating overflow atlas");

        SDL_Texture* newAtlas = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING,
            qrAtlasMaxSize_,
            qrAtlasMaxSize_
        );

        if (!newAtlas) {
            LOG_ERROR("ReloadableGlobalHiscores", "Failed to create overflow atlas");
            SDL_FreeSurface(qrSurf);
            return false;
        }

        SDL_SetTextureBlendMode(newAtlas, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(newAtlas, SDL_ScaleModeNearest);

        void* pixels;
        int pitch;
        if (SDL_LockTexture(newAtlas, nullptr, &pixels, &pitch) == 0) {
            memset(pixels, 0, pitch * qrAtlasMaxSize_);
            SDL_UnlockTexture(newAtlas);
        }

        qrAtlasTextures_.push_back(newAtlas);
        currentAtlasIndex_++;
        qrAtlasHeight_ = qrAtlasMaxSize_;

        qrNextX_ = padding;
        qrNextY_ = padding;

        LOG_INFO("ReloadableGlobalHiscores",
            "Created overflow atlas " + std::to_string(currentAtlasIndex_) + " (total: " +
            std::to_string(qrAtlasTextures_.size()) + " atlases)");
    }

    // Add QR to current atlas
    SDL_Rect dstRect = { qrNextX_, qrNextY_, qrSize, qrSize };
    SDL_UpdateTexture(qrAtlasTextures_[currentAtlasIndex_], &dstRect, qrSurf->pixels, qrSurf->pitch);

    SDL_FreeSurface(qrSurf);

    qrAtlasMap_[gameId] = { dstRect, gameId, currentAtlasIndex_ };
    qrNextX_ += cellSize;

    LOG_INFO("ReloadableGlobalHiscores",
        "Added QR to atlas " + std::to_string(currentAtlasIndex_) + ": " + gameId +
        " (total: " + std::to_string(qrAtlasMap_.size()) + " QRs)");

    return true;
}

const ReloadableGlobalHiscores::QrAtlasEntry*
ReloadableGlobalHiscores::findQrInAtlas_(const std::string& gameId) const {
    auto it = qrAtlasMap_.find(gameId);
    return (it != qrAtlasMap_.end()) ? &it->second : nullptr;
}

void ReloadableGlobalHiscores::beginContext_(bool resetQr) {
    pagePhase_ = PagePhase::Single;
    pageT_ = 0.f;

    if (resetQr) {
        qrPhase_ = QrPhase::Hidden;
        qrT_ = 0.f;
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
        LOG_ERROR("ReloadableGlobalHiscores",
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

	clearQrAtlas_();

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
    // Replace old QR loading with:
    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (renderer) {
        buildInitialAtlas_(renderer);
    }

    tablesNeedRedraw_ = true;
    needsRedraw_ = true;
    reloadTexture();
}

void ReloadableGlobalHiscores::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    clearQrAtlas_();

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
            if (qrT_ >= qrFadeSec_) {
                qrPhase_ = QrPhase::Visible;
                qrT_ = qrFadeSec_;
            }
            needsRedraw_ = true;  // Re-render every frame for fade
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

    // === OPTIMIZATION: Skip double-blend when steady-state ===
    if (pagePhase_ == PagePhase::Single) {
        // Defensive cleanup (should already be null from update())
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }
        // No crossfade, just render intermediate directly
        SDL::renderCopyF(intermediateTexture_, baseAlpha,
            nullptr, &rect, baseViewInfo, layoutW, layoutH);
        return;
    }

    // Compute alphas for page crossfade
    float prevCompA, newCompA;
    computeAlphas_(baseAlpha, prevCompA, newCompA);

    // === Pre-composite during crossfade ===
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

            // Single render of crossfaded result
            SDL::renderCopyF(crossfadeTexture_, baseAlpha,
                nullptr, &rect, baseViewInfo, layoutW, layoutH);

            return;
        }
    }

    // === FALLBACK: Direct render (SnapshotPending or crossfade texture failed) ===

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

void ReloadableGlobalHiscores::TextBatch::clear() {
    vertices.clear();
    indices.clear();
    mip = nullptr;
}

void ReloadableGlobalHiscores::TextBatch::addQuad(const SDL_FRect& dst, const SDL_Rect& src,
    int atlasW, int atlasH, SDL_Color color) {
    int base = vertices.size();

    float u0 = (float)src.x / atlasW;
    float v0 = (float)src.y / atlasH;
    float u1 = (float)(src.x + src.w) / atlasW;
    float v1 = (float)(src.y + src.h) / atlasH;

    vertices.push_back({ {dst.x, dst.y}, color, {u0, v0} });
    vertices.push_back({ {dst.x + dst.w, dst.y}, color, {u1, v0} });
    vertices.push_back({ {dst.x + dst.w, dst.y + dst.h}, color, {u1, v1} });
    vertices.push_back({ {dst.x, dst.y + dst.h}, color, {u0, v1} });

    indices.insert(indices.end(), {
        base + 0, base + 1, base + 2,
        base + 0, base + 2, base + 3
        });
}

void ReloadableGlobalHiscores::TextBatch::render(SDL_Renderer* r, SDL_Texture* texture) {
    if (vertices.empty()) return;
    SDL_RenderGeometry(r, texture,
        vertices.data(), vertices.size(),
        indices.data(), indices.size());
}

void ReloadableGlobalHiscores::reloadTexture() {
    // helper lambdas
    auto decodeUTF8 = [](const char*& ptr, const char* end) -> Uint32 {
        if (ptr >= end) return 0;

        unsigned char c = *ptr++;

        // ASCII (1 byte)
        if (c < 0x80) {
            return c;
        }
        // 2-byte sequence
        else if ((c & 0xE0) == 0xC0) {
            if (ptr >= end) return 0;
            return ((c & 0x1F) << 6) | (*ptr++ & 0x3F);
        }
        // 3-byte sequence
        else if ((c & 0xF0) == 0xE0) {
            if (ptr + 1 >= end) return 0;
            Uint32 cp = ((c & 0x0F) << 12) | ((*ptr++ & 0x3F) << 6);
            return cp | (*ptr++ & 0x3F);
        }
        // 4-byte sequence
        else if ((c & 0xF8) == 0xF0) {
            if (ptr + 2 >= end) return 0;
            Uint32 cp = ((c & 0x07) << 18) | ((*ptr++ & 0x3F) << 12);
            cp |= ((*ptr++ & 0x3F) << 6);
            return cp | (*ptr++ & 0x3F);
        }

        return 0; // Invalid UTF-8
        };

    auto renderTextOutlined = [&](SDL_Renderer* r, FontManager* f,
        const std::string& s, float x, float y, float finalScale,
        TextBatch* outlineBatch, TextBatch* fillBatch) {

            if (s.empty()) return;

            const int outline = f->getOutlinePx();
            const float targetH = finalScale * (f->getMaxHeight() + 2.0f * outline);
            const FontManager::MipLevel* mip = f->getMipLevelForSize((int)targetH);
            if (!mip || !mip->fillTexture) return;

            const float k = (mip->height > 0)
                ? (targetH / float(mip->height + 2 * f->getOutlinePx()))
                : 1.0f;

            const float ySnap = std::round(y);

            if (outlineBatch && !outlineBatch->mip) outlineBatch->mip = mip;
            if (fillBatch && !fillBatch->mip) fillBatch->mip = mip;

            SDL_Color white = { 255, 255, 255, 255 };
            SDL_Color outlineColor = { 0, 0, 0, 255 };

            float penX = x;
            Uint32 prev = 0;

            const char* ptr = s.c_str();
            const char* end = ptr + s.size();

            while (ptr < end) {
                Uint32 ch = decodeUTF8(ptr, end);
                if (ch == 0) break;

                if (prev) penX += f->getKerning(prev, ch) * finalScale;

                auto it = mip->glyphs.find(ch);
                if (it != mip->glyphs.end()) {
                    const auto& g = it->second;

                    if (outlineBatch && mip->outlineTexture) {
                        SDL_FRect dst = {
                            penX - g.fillX * k,
                            ySnap - g.fillY * k,
                            g.rect.w * k,
                            g.rect.h * k
                        };
                        outlineBatch->addQuad(dst, g.rect, mip->atlasW, mip->atlasH, outlineColor);
                    }

                    if (fillBatch) {
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
                        fillBatch->addQuad(dst, srcFill, mip->atlasW, mip->atlasH, white);
                    }

                    penX += g.advance * k;
                }
                prev = ch;
            }
        };

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
        Uint32 prev = 0;

           const char* ptr = s.c_str();
    const char* end = ptr + s.size();
    
    while (ptr < end) {
        Uint32 ch = decodeUTF8(ptr, end);
        if (ch == 0) break;
        
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
            case ColAlign::Left:   return x + 1.0f;
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
    if (!highScoreTable_ || highScoreTable_->tables.empty()) {
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

    const int totalTables = (int)highScoreTable_->tables.size();
    const float compW = baseViewInfo.Width;
    const float compH = baseViewInfo.Height;

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font) {
        needsRedraw_ = true;
        return;
    }

    const float baseScale = baseViewInfo.FontSize / (float)font->getMaxHeight();
    const float asc = (float)font->getMaxAscent();

    const float drawableH0 = asc * baseScale;
    const float lineH0 = drawableH0 * (1.0f + baseRowPadding_);
    const float colPad0 = baseColumnPadding_ * drawableH0;

    constexpr float kPanelGuardPx = 1.0f;

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

    if (pagePhase_ == PagePhase::SnapshotPending) {
        snapshotPrevPage_(renderer, compositeW, compositeH);
    }
    else if (pagePhase_ == PagePhase::Single) {
        if (prevCompositeTexture_) {
            SDL_DestroyTexture(prevCompositeTexture_);
            prevCompositeTexture_ = nullptr;
        }
    }

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

    std::vector<std::string> gameIds;
    if (selectedItem && !selectedItem->iscoredId.empty()) {
        Utils::listToVector(selectedItem->iscoredId, gameIds, ',');
    }

    for (const auto& id : gameIds) {
        if (!id.empty() && !findQrInAtlas_(id)) {
            addQrToAtlas_(renderer, id);
        }
    }

    std::vector<const QrAtlasEntry*> qrEntries(Nvisible, nullptr);
    std::vector<std::pair<int, int>> qrSizes(Nvisible, { 0, 0 });

    for (int t = 0; t < Nvisible; ++t) {
        const int gi = startIdx + t;
        if (gi < (int)gameIds.size() && !gameIds[gi].empty()) {
            const QrAtlasEntry* entry = findQrInAtlas_(gameIds[gi]);
            if (entry) {
                qrEntries[t] = entry;
                qrSizes[t] = { 58, 58 };
            }
        }
    }

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

        float h = lineH0;
        if (!table.id.empty()) h += lineH0;
        h += lineH0 * kRowsPerPage;
        height0[t] = h;
    }

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
    // STAGE 1: Render tables to tableTexture_ (ONLY when dirty)
    // ========================================================================

    if (tablesNeedRedraw_) {
        // Rebuild layouts when table data changes
        slotLayouts_.clear();
        slotLayouts_.reserve(Nvisible);

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

            std::vector<float> colW(maxCols, 0.0f);
            float totalWCols = 0.0f;
            for (size_t c = 0; c < maxCols; ++c) {
                colW[c] = maxColW0[c] * ratio;
                totalWCols += colW[c];
                if (c + 1 < maxCols) totalWCols += colPad;
            }
            totalWCols += 2.0f * kPanelGuardPx;

            const float titleH = table.id.empty() ? 0.0f : lineH;
            const float headerH = lineH;
            const float dataH = lineH * kRowsPerPage;
            const float panelH = titleH + headerH + dataH;
            const float panelW = totalWCols;

            const int extraL = (int)std::lround(qrReserve[t].L);
            const int extraR = (int)std::lround(qrReserve[t].R);
            const int extraT = (int)std::lround(qrReserve[t].T);
            const int extraB = (int)std::lround(qrReserve[t].B);

            float anchorW = panelW + (float)(extraL + extraR);
            float anchorH = panelH + (float)(extraT + extraB);

            float anchorX = xCell + (cellW - anchorW) * 0.5f;
            float anchorY = yCell;
            anchorX = std::round(clampf(anchorX, xCell, xCell + cellW - anchorW));
            anchorY = std::round(clampf(anchorY, yCell, yCell + cellH - anchorH));

            float drawX0 = anchorX + (float)extraL + kPanelGuardPx;
            float drawY0 = anchorY + (float)extraT;

            SlotLayout layout;
            layout.panelRect = { drawX0 - kPanelGuardPx, drawY0, panelW, panelH };
            layout.xCell = xCell;
            layout.yCell = yCell;
            layout.finalScale = finalScale;
            layout.lineH = lineH;
            layout.colW = colW;
            layout.colPad = colPad;
            layout.drawX0 = drawX0;
            layout.drawY0 = drawY0;
            layout.hasQr = false;

            // Pre-calculate QR destination
            const auto& [qrW, qrH] = qrSizes[t];
            if (qrW > 0 && qrH > 0) {
                float qrX = 0.f, qrY = 0.f;
                const SDL_FRect& panelRect = layout.panelRect;

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

                layout.qrDst = { std::round(qrX), std::round(qrY), (float)qrW, (float)qrH };
                layout.hasQr = qrEntries[t] && qrSizes[t].first > 0;
            }

            slotLayouts_.push_back(layout);
        }
    }

    if (tablesNeedRedraw_) {
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

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        TextBatch outlineBatch, fillBatch;

        // Render all tables using cached layouts
        for (int t = 0; t < Nvisible; ++t) {
            const int gi = startIdx + t;
            const auto& table = highScoreTable_->tables[gi];
            const auto& layout = slotLayouts_[t];

            float y = layout.drawY0;

            // ---- Title ----
            if (!table.id.empty()) {
                float w = measureTextWidthExact(font, table.id, layout.finalScale);
                float totalW = layout.panelRect.w;
                float x = std::round((totalW - w) * 0.5f);
                renderTextOutlined(renderer, font, table.id, layout.drawX0 + x, y, layout.finalScale,
                    &outlineBatch, &fillBatch);
                y += layout.lineH;
            }

            // ---- Headers ----
            {
                float x = 0.0f;
                for (size_t c = 0; c < maxCols; ++c) {
                    if (c < table.columns.size()) {
                        const std::string& header = table.columns[c];
                        float hw = measureTextWidthExact(font, header, layout.finalScale);
                        float xAligned = std::round(layout.drawX0 + x + (layout.colW[c] - hw) * 0.5f);
                        renderTextOutlined(renderer, font, header, xAligned, y, layout.finalScale,
                            &outlineBatch, &fillBatch);
                    }
                    x += layout.colW[c];
                    if (c + 1 < maxCols) x += layout.colPad;
                }
                y += layout.lineH;
            }

            // ---- Rows ----
            for (int r = 0; r < kRowsPerPage; ++r) {
                float x = 0.0f;
                const auto* rowV = (r < (int)table.rows.size()) ? &table.rows[r] : nullptr;
                for (size_t c = 0; c < maxCols; ++c) {
                    std::string cell;
                    if (rowV && c < rowV->size()) cell = (*rowV)[c];
                    const float tw = measureTextWidthExact(font, cell, layout.finalScale);
                    const bool ph = (r < (int)table.isPlaceholder.size() &&
                        c < table.isPlaceholder[r].size())
                        ? table.isPlaceholder[r][c]
                        : false;
                    const ColAlign a = ph ? ColAlign::Center : colAlignFor(c, maxCols);
                    const float xAligned = alignX(layout.drawX0 + x, layout.colW[c], tw, a);
                    if (!cell.empty()) {
                        renderTextOutlined(renderer, font, cell, std::round(xAligned), y, layout.finalScale,
                            &outlineBatch, &fillBatch);
                    }
                    x += layout.colW[c];
                    if (c + 1 < maxCols) x += layout.colPad;
                }
                y += layout.lineH;
            }
        }

        // Render batches
        if (outlineBatch.mip && outlineBatch.mip->outlineTexture) {
            outlineBatch.render(renderer, outlineBatch.mip->outlineTexture);
        }

        if (fillBatch.mip && fillBatch.mip->fillTexture) {
            SDL_Color c = font->getColor();
            SDL_SetTextureColorMod(fillBatch.mip->fillTexture, c.r, c.g, c.b);
            fillBatch.render(renderer, fillBatch.mip->fillTexture);
        }

        SDL_SetRenderTarget(renderer, old);
        tablesNeedRedraw_ = false;
    }

    // ========================================================================
    // STAGE 2: Composite tableTexture_ + QRs → intermediateTexture_
    //          (Uses cached QR positions from slotLayouts)
    // ========================================================================

    if (!intermediateTexture_) {
        needsRedraw_ = true;
        return;
    }

    SDL_Texture* old = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, intermediateTexture_);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Copy cached table texture (full alpha)
    if (tableTexture_) {
        SDL_RenderCopy(renderer, tableTexture_, nullptr, nullptr);
    }

    // Draw QRs with dynamic alpha
    if (qrPhase_ == QrPhase::FadingIn || qrPhase_ == QrPhase::Visible) {
        float qrAlpha = 1.0f;
        if (qrPhase_ == QrPhase::FadingIn) {
            qrAlpha = std::clamp(qrT_ / qrFadeSec_, 0.f, 1.f);
        }

        const Uint8 alpha = (Uint8)(qrAlpha * 255);

        if (!qrAtlasTextures_.empty()) {
            for (int t = 0; t < Nvisible; ++t) {
                const auto& layout = slotLayouts_[t];
                if (!layout.hasQr) continue;

                const QrAtlasEntry* entry = qrEntries[t];
                if (!entry) continue;

                if (entry->atlasIndex < 0 || entry->atlasIndex >= (int)qrAtlasTextures_.size()) continue;

                SDL_Texture* atlasTexture = qrAtlasTextures_[entry->atlasIndex];
                if (!atlasTexture) continue;

                SDL_SetTextureAlphaMod(atlasTexture, alpha);

                // Use pre-calculated QR destination from layout
                SDL_RenderCopyF(renderer, atlasTexture, &entry->srcRect, &layout.qrDst);
            }
        }
    }

    SDL_SetRenderTarget(renderer, old);

    needsRedraw_ = false;
}