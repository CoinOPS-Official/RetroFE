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
#include <SDL_image.h>

#include "Component.h"
#include "../../Collection/Item.h"
#include "../../Database/HiScores.h"

 // fwd-declare to avoid pulling the whole font header here (optional)
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

	enum class QrPlacement {
		TopCentered, TopRight, TopLeft, BottomRight, BottomLeft, BottomCenter, RightMiddle, LeftMiddle,
	};

	struct QrEntry {
		SDL_Texture* tex = nullptr;
		int w = 0;
		int h = 0;
		bool ok = false;
	};

	// --- QR config/state ---
	QrPlacement qrPlacement_ = QrPlacement::TopLeft;
	int qrMarginPx_ = 6;                  // fixed pixel gap between panel and QR
	std::vector<QrEntry> qrByTable_;      // one per table (aligned with tables)

	// --- Helpers (implemented in .cpp) ---
	void destroyAllQr_();

	struct PlannedDraw {
		SDL_FRect dst{}; // precomputed destination within the component rect
		float headerTopLocal = 0.0f; // local Y of header top (for QR placement)
	};
	std::vector<PlannedDraw> planned_;

	void reloadTexture(bool reset = true);

	// --- Config ---
	FontManager* fontInst_;
	std::string  textFormat_;
	float        baseColumnPadding_;
	float        baseRowPadding_;
	int          displayOffset_;

	// --- State/Resources ---
	bool            needsRedraw_ = true;
	Item* lastSelectedItem_ = nullptr;
	HighScoreData* highScoreTable_ = nullptr;
	SDL_Texture* intermediateTexture_ = nullptr;

	// --- Grid rendering ---
	static constexpr int kRowsPerPage = 10;

	struct PageTex {
		SDL_Texture* tex = nullptr;
		int w = 0;
		int h = 0;
	};
	std::vector<PageTex> tablePanels_;   // one texture per hiscore table

	// Grid hints (can be wired to XML later)
	int   gridColsHint_ = 0;     // 0 = auto near-square
	float cellSpacingH_ = 0.02f; // fraction of width
	float cellSpacingV_ = 0.02f; // fraction of height


};