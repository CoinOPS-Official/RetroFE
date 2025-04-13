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

#include <unordered_map>
#include <string>
#include <vector>
#include <shared_mutex>
#include "../Utility/Utils.h"
#include "../Database/Configuration.h"

struct HighScoreTable {
    std::string id;  // Table ID, if any
    std::vector<std::string> columns;  // Column names
    std::vector<std::vector<std::string>> rows;  // Rows of cell values
	bool forceRedraw = false;  // Force redraw of high score table
};

struct HighScoreData {
    std::vector<HighScoreTable> tables;  // All tables for a game
};

class HiScores {
public:
    static HiScores& getInstance();

    // Initialize by loading all high scores
    void loadHighScores(const std::string& zipPath, const std::string& overridePath);

    HighScoreData* getHighScoreTable(const std::string& gameName);

    bool hasHiFile(const std::string& gameName) const;

    bool runHi2Txt(const std::string& gameName);

    void runHi2TxtAsync(const std::string& gameName);

    bool loadFileToBuffer(const std::string& filePath, std::vector<char>& buffer);


private:
    HiScores() = default;

    std::string hiFilesDirectory_;
    std::string scoresDirectory_;

    std::unordered_map<std::string, HighScoreData> scoresCache_;
    std::shared_mutex scoresCacheMutex_;
    void loadFromZip(const std::string& zipPath);
    void loadFromFile(const std::string& gameName, const std::string& filePath, std::vector<char>& buffer);
};

