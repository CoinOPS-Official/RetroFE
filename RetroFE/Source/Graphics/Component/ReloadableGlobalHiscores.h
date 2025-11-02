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
#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <SDL2/SDL.h>

#include "Component.h"

 // Forward declarations
class Configuration;
class FontManager;
struct HighScoreData;

class ReloadableGlobalHiscores : public Component {
public:
    ReloadableGlobalHiscores(Configuration& config, std::string textFormat,
        Page& p, int displayOffset,
        FontManager* font,
        float baseColumnPadding, float baseRowPadding);
    ~ReloadableGlobalHiscores() override;

    bool  update(float dt) override;
    void  draw() override;
    void  allocateGraphicsMemory() override;
    void  freeGraphicsMemory() override;
    void  deInitializeFonts() override;
    void  initializeFonts() override;

private:
    // --- Enums for state management ---

    // Fade state for the composite/page
    enum class PagePhase {
        Single,           // not crossfading
        SnapshotPending,  // crossfade requested, waiting for snapshot
        Crossfading       // actively fading prev ? current
    };

    // Fade state for QR overlay
    enum class QrPhase {
        Hidden,       // table hidden (baseViewInfo.Alpha == 0)
        WaitingDelay, // visible, counting the delay before first appearance
        FadingIn,     // first-time delayed fade-in
        Visible       // steady-state (after the one-time delay)
    };

    // QR code placement options
    enum class QrPlacement {
        TopCentered,
        TopRight,
        TopLeft,
        BottomRight,
        BottomLeft,
        BottomCenter,
        RightMiddle,
        LeftMiddle
    };

    // --- Core rendering ---
    void reloadTexture();

    // --- Grid baseline computation ---
    void computeGridBaseline_(FontManager* font,
        int totalTables, float compW, float compH,
        float baseScale, float asc);

    // --- State helpers ---
    void beginContext_(bool resetQr = true);      // new game/score context; reset everything
    void beginPageFlip_();     // enter Crossfading; snapshot happens in reloadTexture()
    void computeAlphas_(float baseA,
        float& prevCompA, float& newCompA) const;  // CHANGED: removed QR alphas

    // One-shot snapshot helper used during crossfading
    void snapshotPrevPage_(SDL_Renderer* r, int compositeW, int compositeH);

    // --- Change detection ---
    std::string cachedIscoredId_;                    // last iscoredId string we processed
    std::vector<std::string> cachedIds_;             // parsed ids from that string
    std::unordered_map<std::string, uint64_t> lastSeenHashes_;  // gameId - last hash we rendered
    Item const* lastSelectedItem_ = nullptr;

    // --- Config / Font ---
    FontManager* fontInst_;
    std::string  textFormat_;
    float        baseColumnPadding_;
    float        baseRowPadding_;
    int          displayOffset_;

    // --- State / Resources ---

    bool           tablesNeedRedraw_;    // Tables need re-rendering (data/geometry changed)
    bool           needsRedraw_;
    HighScoreData* highScoreTable_;

    // Composite textures
    SDL_Texture* tableTexture_;             // Tables only (cached until data/geometry changes)
    SDL_Texture* intermediateTexture_;      // current page (tables + QRs composited together)
    SDL_Texture* prevCompositeTexture_;     // previous page snapshot
    SDL_Texture* crossfadeTexture_;  // Pre-composited crossfade result

    // --- Geometry cache ---
    float prevGeomW_;
    float prevGeomH_;
    float prevGeomFont_;
    int   compW_;               // composite texture width
    int   compH_;               // composite texture height

    // --- Grid rendering constants ---
    static constexpr int kRowsPerPage = 10;

    // --- Grid configuration ---
    int   gridColsHint_;        // 0 = auto near-square layout
    float cellSpacingH_;        // horizontal spacing as fraction of width
    float cellSpacingV_;        // vertical spacing as fraction of height
    int   gridPageSize_;        // tables per page
    float gridRotatePeriodSec_; // seconds before auto-flip to next page

    // --- Grid state ---
    float gridTimerSec_;        // countdown to next page flip
    int   gridPageIndex_;       // current page index

    // --- Grid layout baseline (computed per-geometry) ---
    int   gridBaselineSlots_;   // slots per page
    int   gridBaselineCols_;    // columns in grid
    int   gridBaselineRows_;    // rows in grid
    float gridBaselineCellW_;   // cell width
    float gridBaselineCellH_;   // cell height
    std::vector<float> gridBaselineRowMin_;  // per-row minimum scale
    bool  gridBaselineValid_;   // true if baseline is computed

    // --- Debounce ---
    float reloadDebounceTimer_; // countdown in seconds
    float reloadDebounceSec_;   // debounce duration

    // --- Page state ---
    PagePhase pagePhase_;       // current page phase
    float     pageT_;           // crossfade timer (0 to pageDurationSec_)
    float     pageDurationSec_; // crossfade duration

    // --- QR state ---
    QrPhase   qrPhase_;         // current QR phase
    float     qrT_;             // QR delay/fade timer
    float     qrDelaySec_;      // delay before QR first appears
    float     qrFadeSec_;       // QR fade-in duration

    // QR surface cache (loaded once, reused during fade)
    std::vector<SDL_Texture*> cachedQrTextures_;
    std::vector<std::pair<int, int>> cachedQrSizes_;
    std::vector<std::string> cachedQrGameIds_;

    // --- QR configuration ---
    QrPlacement qrPlacement_;   // where to place QR codes
    int         qrMarginPx_;    // margin between QR and panel
};