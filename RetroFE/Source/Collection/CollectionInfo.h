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
#include <unordered_map>

class Item;
class Configuration;

class CollectionInfo
{
public:
    CollectionInfo(Configuration& c, std::string name, std::string listPath, std::string extensions, std::string metadataType, std::string metadataPath);
    virtual ~CollectionInfo();
    std::string settingsPath() const;
    bool Save();
    void sortItems();
    void sortPlaylists();
    void addSubcollection(CollectionInfo *info);
    static auto itemIsLess(std::string sortType);
    void extensionList(std::vector<std::string> &extensions);
    std::string name;
    std::string lowercaseName();
    std::string listpath;
    bool saveRequest;
    std::string metadataType;
    std::string launcher;
    std::vector<Item *> items;
    std::vector<Item*> playlistItems;

    typedef std::map<std::string, std::vector <Item *> *> Playlists_T;
    Playlists_T playlists;
    std::string sortType;
    bool menusort;
    bool subsSplit;
    bool hasSubs;
    bool sortDesc;

    std::map<std::string, std::vector<std::string>> playlistOrders;
    void readPlaylistFile(const std::string& playlistName);
    std::vector<std::string> playlistOrder;
    void readAllPlaylistFiles();
    void customSortAllItems();
    int findInPlaylistOrder(const std::string& itemName, const std::vector<std::string>& playlistOrder);
    void customSortPlaylist(const std::string& playlistName, std::vector<Item*>* playlist);
    void customSort(std::vector<Item*>& itemsToSort, const std::unordered_map<std::string, std::size_t>& orderIndices, const std::string& mainCollectionName);

private:
    Configuration& conf_;
    std::string metadataPath_;
    std::string extensions_;
};
