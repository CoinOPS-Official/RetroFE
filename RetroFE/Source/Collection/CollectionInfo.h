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
#include <vector>
#include <map>

class Item;
class Configuration;

class CollectionInfo
{
public:
    CollectionInfo(Configuration& c, const std::string& name, const std::string& listPath, const std::string &extensions, const std::string& metadataType, const std::string& metadataPath);
    virtual ~CollectionInfo();
    std::string settingsPath() const;
    bool saveFavorites(Item* removed = nullptr);
    void sortItems();
    void sortPlaylists();
    bool isItemInLastPlayed(const Item* selectedItem);
    void addSubcollection(CollectionInfo *info);
    auto itemIsLess(const std::string& sortType, bool currentCollectionMenusort) const;
    void extensionList(std::vector<std::string> &extensions) const;
    std::string name;
    std::string lowercaseName() const;
    std::string listpath;
    bool saveRequest;
    std::string metadataType;
    std::string launcher;
    std::vector<Item *> items;
    std::vector<Item*> playlistItems;

    using Playlists_T = std::map<std::string, std::vector<Item*>*, std::less<>>;
    Playlists_T playlists;
    std::string sortType;
    bool menusort;
    bool subsSplit;
    bool hasSubs;
    bool sortDesc;
private:
    Configuration& conf_;
    std::string metadataPath_;
    std::string extensions_;
};
