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
#include <SDL2/SDL.h>

#include "Component.h"
#include "../../Collection/Item.h"
#include "../../Database/HiScores.h"

 // fwd-declare to avoid pulling the whole font header here (optional)
class Configuration;
class FontManager;

class ReloadableGlobalHiscores : public Component {
public:
	// Simplified ctor: drop scrolling/startTime
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
    void  reloadTexture(); // full-bounds composite repaint

    // ----- QR handling -----
    enum class QrPlacement {
        TopCentered, TopRight, TopLeft, BottomRight, BottomLeft, BottomCenter, RightMiddle, LeftMiddle,
    };
    struct QrEntry {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
        bool ok = false;
    };
    void destroyAllQr_();

    // ----- Grid baseline (computed per-geometry) -----
    void computeGridBaseline_(SDL_Renderer* renderer, FontManager* font,
        int totalTables, float compW, float compH,
        float baseScale, float asc);

    // ----- Config -----
    FontManager* fontInst_;
    std::string  textFormat_;
    float        baseColumnPadding_;
    float        baseRowPadding_;
    int          displayOffset_;

    // ----- State / Resources -----
    bool           needsRedraw_ = true;
    HighScoreData* highScoreTable_ = nullptr;

    // Single full-bounds composite and previous snapshot for crossfade
    SDL_Texture* intermediateTexture_ = nullptr;
    SDL_Texture* prevCompositeTexture_ = nullptr;

    // Geometry caches (per-instance; replace old statics)
    float prevGeomW_ = -1.f, prevGeomH_ = -1.f, prevGeomFont_ = -1.f;
    int   compW_ = 0, compH_ = 0;

    // Data freshness
    uint64_t lastEpochSeen_ = 0;

    // ----- Grid rendering -----
    static constexpr int kRowsPerPage = 10;

    // Grid hints (can be wired to XML later)
    int   gridColsHint_ = 0;     // 0 = auto near-square
    float cellSpacingH_ = 0.02f; // fraction of width
    float cellSpacingV_ = 0.02f; // fraction of height

    // Grid rotation (paging) state
    int   gridPageSize_ = 6;     // show up to 6 tables at once
    float gridRotatePeriodSec_ = 8.0f;  // seconds before flipping to next page
    float gridTimerSec_ = 0.0f;
    int   gridPageIndex_ = 0;     // which slice we are showing (0-based)

    // Grid layout & scale baseline (applies across pages until geometry/data changes)
    int                 gridBaselineSlots_ = 0;    // slots per page (<=6 or totalTables if <6)
    int                 gridBaselineCols_ = 0;
    int                 gridBaselineRows_ = 0;
    float               gridBaselineCellW_ = 0.f;
    float               gridBaselineCellH_ = 0.f;
    std::vector<float>  gridBaselineRowMin_;        // per-row min scale
    bool                gridBaselineValid_ = false;

    // Debounce
    float reloadDebounceTimer_ = 0.0f;   // counts down
    float reloadDebounceSec_ = 0.08f;  // ~80ms; tune as needed

    // Fade state
    bool  fadeActive_ = false;   // are we crossfading?
    bool  fadeStartPending_ = false;   // capture old page in reloadTexture()
    float fadeT_ = 0.0f;    // elapsed fade time (sec)
    float fadeDurationSec_ = 1.0f;    // 1s crossfade (tune)
    bool  hasShownOnce_ = false;

    // QR config/state (aligned to visible tables per page)
    QrPlacement              qrPlacement_ = QrPlacement::TopLeft;
    int                      qrMarginPx_ = 6;         // fixed gap between panel and QR
    std::vector<QrEntry>     qrByTable_;               // one per visible table
};