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

#include <string>
#include <map>
#include "CollectionInfo.h"

class Item
{
public:
    Item() = default;
    virtual ~Item();
    std::string filename() const;
    std::string lowercaseTitle() const;
    std::string lowercaseFullTitle() const;
    std::string getMetaAttribute(const std::string& attribute) const;
    std::string name;
    std::string filepath;
    std::string file{ "" };
    std::string title;
    std::string fullTitle;
    std::string year;
    std::string manufacturer;
    std::string developer;
    std::string genre;
    std::string cloneof;
    std::string numberPlayers;
    std::string numberButtons;
    std::string ctrlType;
    std::string joyWays;
    std::string rating;
    std::string score;
    std::string playlist;
    std::string lastPlayed{ "0" };
    int playCount{ 0 };
    bool        isFavorite{ false };
    CollectionInfo* collectionInfo{ nullptr };
    bool leaf{ true };

    using InfoType = std::map<std::string, std::string, std::less<>>;
    using InfoPair = std::pair<std::string, std::string>;
    InfoType info_;
    void setInfo( std::string key, std::string value );
    bool getInfo(const std::string& key, std::string& value);
    void loadInfo(const std::string& path);
    bool static validSortType(std::string attribute);
    bool static isSortDesc(std::string attribute);
};
