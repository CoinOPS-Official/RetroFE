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

#include "CollectionInfo.h"
#include "../Database/MetadataDatabase.h"
#include <string>
#include <map>
#include <vector>

class Configuration;
class CollectionInfo;


class CollectionInfoBuilder
{
public:
    CollectionInfoBuilder(Configuration &c, MetadataDatabase &mdb);
    virtual ~CollectionInfoBuilder();
    CollectionInfo *buildCollection(const std::string& collectionName);
    CollectionInfo *buildCollection(const std::string& collectionName, const std::string& mergedCollectionName);
    void addPlaylists(CollectionInfo *info);
    void loadPlaylistItems(CollectionInfo* info, std::map<std::string, Item*>* playlistItems, const std::string& path);
    void updateLastPlayedPlaylist(CollectionInfo *info, Item *item, int size);
    void updateTimeSpent(Item* item, double timePlayedInSeconds);
    void injectMetadata(CollectionInfo *info);
    static bool createCollectionDirectory(const std::string& collectionName, const std::string& collectionType = NULL, const std::string& osType = NULL);
    bool ImportBasicList(CollectionInfo *info, const std::string& file, std::vector<Item *> &list);

private:
    Configuration &conf_;
    MetadataDatabase &metaDB_;
    bool ImportBasicList(CollectionInfo *info, const std::string& file, std::map<std::string, Item *> &list);
    bool ImportDirectory(CollectionInfo *info, const std::string& mergedCollectionName);
    std::string getKey(Item* item);
    void AddToPlayCount(Item* item);
    std::map<std::string, double> ImportTimeSpent(const std::string& file);
    std::map<std::string, Item*> ImportPlayCount(const std::string& file);
    void ImportRomDirectory(const std::string& path, CollectionInfo *info, std::map<std::string, Item *> includeFilter, std::map<std::string, Item *> excludeFilter, bool romHierarchy, bool emuarc);
};
