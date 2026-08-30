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
#include <filesystem>
#include <unordered_set>

#include <SDL.h>

#include "Component.h"
#include "../../Collection/Item.h"
#include "../../Database/LocalHiScores.h"
#include "LocalScorePagePlan.h"


class ReloadableHiscores : public Component
{
public:
    ReloadableHiscores(Configuration& config, std::string textFormat, Page& p, int displayOffset, FontManager* font,
        float scrollingSpeed, float startTime, std::string excludedColumns, float baseColumnPadding, float baseRowPadding, size_t maxRows);
    ~ReloadableHiscores() override;
    bool     update(float dt) override;
    void     draw() override;
    void     allocateGraphicsMemory() override;
    void     freeGraphicsMemory() override;
    void     deInitializeFonts() override;
    void     initializeFonts() override;


private:
    struct PagePanel {
        size_t tableIndex = 0;
        std::vector<size_t> visibleColumns;
        std::vector<float> columnWidths;
        std::vector<SDL_Texture*> rowTiles;
        SDL_Texture* header = nullptr;
        float x = 0.0f;
        float width = 0.0f;
        float scale = 0.0f;
        float lineStep = 0.0f;
        float headerHeight = 0.0f;
        float rowsHeight = 0.0f;
        float maxScroll = 0.0f;
        int rowsPerTile = 32;
    };

    struct PreviousPanelState {
        size_t tableIndex = 0;
        std::vector<size_t> visibleColumns;
        std::vector<float> columnWidths;
        float x = 0.0f;
        float width = 0.0f;
        float scale = 0.0f;
        float lineStep = 0.0f;
        float headerHeight = 0.0f;
    };

    bool updatePages_(float dt);
    void rebuildPagePlan_();
    void buildCurrentPage_(bool resetPresentation = true);
    void freePagePanels_();
    void drawPages_();
    bool ensureCompositeTexture_(SDL_Renderer* renderer, int width, int height);
    void renderPanel_(SDL_Renderer* renderer, const PagePanel& panel,
        float originX, float originY, Uint8 alpha) const;
    void renderPanels_(SDL_Renderer* renderer, float originX, float originY, Uint8 alpha) const;
    void capturePageTransition_(bool wholePage, const std::vector<size_t>& changedTables = {});
    void beginPageTransition_();
    bool panelGeometryStable_(const std::vector<size_t>& changedTables) const;
    float measureNaturalWidth_(FontManager* font, const HighScoreTableView& table,
        const std::vector<size_t>& columns, float scale) const;
    void reloadTexture(bool resetScroll = true);
    void beginTableTransition_();
    void cancelTableTransition_();
    void renderCurrentTable_(SDL_Renderer* renderer, float originX, float originY, Uint8 alpha) const;
    void renderNoDataMessage(SDL_Renderer* renderer, FontManager* font);
    float computeTableScaleAndWidths(FontManager* font, const HighScoreTableView& table, float& outDrawableHeight, float& outRowPadding, float& outPaddingBetweenColumns, std::vector<float>& outColumnWidths, float& outTotalTableWidth, float widthConstraint, float heightConstraint);
    void updateVisibleColumns(const HighScoreTableView& table);

    void renderHeaderTexture(FontManager* font, const HighScoreTableView& table, float scale, float drawableHeight, float rowPadding, float paddingBetweenColumns, float totalTableWidth);

    void renderTableRowsTexture(FontManager* font, const HighScoreTableView& table, float scale, float drawableHeight, float rowPadding, float paddingBetweenColumns, float totalTableWidth);
    
    // Configuration Parameters
    Configuration& config_;
    FontManager* fontInst_;
    std::string textFormat_;
    std::string excludedColumns_;
    std::unordered_set<std::string> excludedColumnsSet_;
    float baseColumnPadding_;
    float baseRowPadding_;
    int displayOffset_;
    size_t maxRows_;

    // State Variables
    float scrollingSpeed_;
    float currentPosition_;
    float startTime_;
    float waitStartTime_;
    float waitEndTime_;
    size_t currentTableIndex_;
    float tableDisplayTimer_;
    float currentTableDisplayTime_;
    float displayTime_;
    bool needsRedraw_;
    bool tableCrossfading_;
    float tableCrossfadeTimer_;
    float tableCrossfadeDuration_;
    bool showingNoData_;
    float noDataElapsed_;
    bool wasComponentVisible_;

    // Cached Data
    float lastScale_;
    float lastPaddingBetweenColumns_;
    bool cacheValid_;
    size_t cachedTableIndex_;
    std::vector<float> cachedColumnWidths_;
    float cachedTotalTableWidth_;
    std::vector<size_t> visibleColumnIndices_;
    float cachedViewWidth_;           // Stores the width constraint used for the last full calculation
    float cachedViewHeight_ = -1.0f;
    float cachedBaseFontSize_;        // Stores the baseViewInfo.FontSize used
    float lastComputedDrawableHeight_; // Drawable height based on final scale
    float lastComputedRowPadding_;     // Row padding based on final scale

    int currentHeaderW = 0, currentHeaderH = 0;
    int currentRowsW = 0, currentRowsH = 0;
    Uint8 lastAlphaCache_ = 255;

    // Resources
    Item* lastSelectedItem_;
    std::string lastSelectedGame_;
    uint64_t lastRenderedRevision_;
    HighScoreView highScoreTable_;
    std::vector<LocalScorePagePlan> pagePlan_;
    std::vector<PagePanel> pagePanels_;
    size_t currentPageIndex_ = 0;
    float pageElapsed_ = 0.0f;
    float pageEndPause_ = 0.0f;
    SDL_Texture* headerTexture_;
    SDL_Texture* tableRowsTexture_;
    SDL_Texture* previousTableTexture_;
    std::vector<PreviousPanelState> previousPanelStates_;
    std::unordered_set<size_t> transitioningTableIndices_;
    bool wholePageTransition_ = false;
    SDL_Texture* compositeTexture_;
    int compositeTextureWidth_;
    int compositeTextureHeight_;
    int tableRowsTextureHeight_;
	int headerTextureHeight_;
};
