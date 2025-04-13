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

#include <SDL2/SDL.h>

#include "Component.h"
#include "../../Collection/Item.h"
#include "../../Database/HiScores.h"


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
    void reloadTexture(bool resetScroll = true);
    void cacheColumnWidths(FontManager* font, float scale, const HighScoreTable& table, float paddingBetweenColumns);
    void updateVisibleColumns(const HighScoreTable& table);
    bool createIntermediateTexture(SDL_Renderer* renderer, int width, int height);
    
    // Configuration Parameters
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

    // Cached Data
    float lastScale_;
    float lastPaddingBetweenColumns_;
    bool cacheValid_;
    size_t cachedTableIndex_;
    std::vector<float> cachedColumnWidths_;
    float cachedTotalTableWidth_;
    std::vector<size_t> visibleColumnIndices_;

    // Resources
    Item* lastSelectedItem_;
    HighScoreData* highScoreTable_;
    SDL_Texture* intermediateTexture_;
};
