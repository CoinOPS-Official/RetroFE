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
#include "CollectionInfo.h"
#include "Item.h"
#include "../Database/Configuration.h"
#include "../Utility/Utils.h"
#include "../Utility/Log.h"
#include <sstream>
#include <fstream>
#include <algorithm>
#include <exception>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <dirent.h>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

#if defined(__linux) || defined(__APPLE__)
#include <errno.h>
#include <cstring>
#endif

CollectionInfo::CollectionInfo(
    Configuration& c,
    std::string name,
    std::string listPath,
    std::string extensions,
    std::string metadataType,
    std::string metadataPath)
    : name(name)
    , listpath(listPath)
    , saveRequest(false)
    , metadataType(metadataType)
    , menusort(true)
    , subsSplit(false)
    , hasSubs(false)
    , sortDesc(false)
    , conf_(c)  // Initialize conf_ last
    , metadataPath_(metadataPath)
    , extensions_(extensions)
{
}

CollectionInfo::~CollectionInfo()
{
    Playlists_T::iterator pit = playlists.begin();

    while(pit != playlists.end())
    {
        if(pit->second != &items)
        {
            delete pit->second;
        }
        playlists.erase(pit);
        pit = playlists.begin();
    }

	std::vector<Item *>::iterator it = items.begin();
    while(it != items.end())
    {
        delete *it;
        items.erase(it);
        it = items.begin();
    }
}

bool CollectionInfo::Save() 
{
    bool retval = true;
    if(saveRequest && name != "")
    {
        std::string playlistCollectionName = name;
        bool globalFavLast = false;
        (void)conf_.getProperty("globalFavLast", globalFavLast);
        if (globalFavLast) {
            playlistCollectionName = "Favorites";
        }
        std::string dir  = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists");
        std::string file = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists", "favorites.txt");
        Logger::write(Logger::ZONE_INFO, "Collection", "Saving " + file);

        std::ofstream filestream;
        try
        {
            // Create playlists directory if it does not exist yet.
            struct stat info;
            if ( stat( dir.c_str(), &info ) != 0 )
            {
#if defined(_WIN32) && !defined(__GNUC__)
                if(!CreateDirectory(dir.c_str(), NULL))
                {
                    if(ERROR_ALREADY_EXISTS != GetLastError())
                    {
                        Logger::write(Logger::ZONE_WARNING, "Collection", "Could not create directory " + dir);
                        return false;
                    }
                }
#else 
#if defined(__MINGW32__)
                if(mkdir(dir.c_str()) == -1)
#else
                if(mkdir(dir.c_str(), 0755) == -1)
#endif        
                {
                    Logger::write(Logger::ZONE_WARNING, "Collection", "Could not create directory " + dir);
                    return false;
                }
#endif
            }
            else if ( !(info.st_mode & S_IFDIR) )
            {
                Logger::write(Logger::ZONE_WARNING, "Collection", dir + " exists, but is not a directory.");
                return false;
            }

            filestream.open(file.c_str());
            std::vector<Item *> *saveitems = playlists["favorites"];
            for(std::vector<Item *>::iterator it = saveitems->begin(); it != saveitems->end(); it++)
            {
                if ((*it)->collectionInfo->name == (*it)->name)
                {
                    filestream << (*it)->name << std::endl;
                }
                else
                {
                    filestream << "_" << (*it)->collectionInfo->name << ":" << (*it)->name << std::endl;
                }
            }

            filestream.close();
        }
        catch(std::exception &)
        {
            Logger::write(Logger::ZONE_ERROR, "Collection", "Save failed: " + file);
            retval = false;
        }
    }
    
    return retval;
}


std::string CollectionInfo::settingsPath() const
{
    return Utils::combinePath(Configuration::absolutePath, "collections", name);
}


void CollectionInfo::extensionList(std::vector<std::string> &extensionlist)
{
    std::istringstream ss(extensions_);
    std::string token;

    while(std::getline(ss, token, ','))
    {
        token = Utils::trimEnds(token);
    	extensionlist.push_back(token);
    }
}

std::string CollectionInfo::lowercaseName()
{
    std::string lcstr = name;
    std::transform(lcstr.begin(), lcstr.end(), lcstr.begin(), ::tolower);
    return lcstr;
}

void CollectionInfo::addSubcollection(CollectionInfo *newinfo)
{
    items.insert(items.begin(), newinfo->items.begin(), newinfo->items.end());
}

auto CollectionInfo::itemIsLess(std::string sortType)
{
    return [sortType](Item* lhs, Item* rhs) {

        if (lhs->leaf && !rhs->leaf) return true;
        if (!lhs->leaf && rhs->leaf) return false;

        // sort by collections first
        if (lhs->collectionInfo->subsSplit && lhs->collectionInfo != rhs->collectionInfo)
            return lhs->collectionInfo->lowercaseName() < rhs->collectionInfo->lowercaseName();
        if (!lhs->collectionInfo->menusort && !lhs->leaf && !rhs->leaf)
            return false;

        // sort by another attribute
        if (sortType != "") {
            std::string lhsValue = lhs->getMetaAttribute(sortType);
            std::string rhsValue = rhs->getMetaAttribute(sortType);
            bool desc = Item::isSortDesc(sortType);

            if (lhsValue != rhsValue) {
                return desc ? lhsValue > rhsValue : lhsValue < rhsValue;
            }
        }
        // default sort by name
        return lhs->lowercaseFullTitle() < rhs->lowercaseFullTitle();
    };
}


void CollectionInfo::sortItems()
{
    std::sort( items.begin(), items.end(), itemIsLess(""));
}


void CollectionInfo::sortPlaylists()
{
    std::vector<Item *> *allItems = &items;
    std::vector<Item *> toSortItems;

    for ( Playlists_T::iterator itP = playlists.begin( ); itP != playlists.end( ); itP++ )
    {
        if ( itP->second != allItems )
        {
            // temporarily set collection info's sortType so search has access to it
            sortType = Item::validSortType(itP->first) ? itP->first : "";
            std::sort(itP->second->begin(), itP->second->end(), itemIsLess(sortType));
        }
    }
    sortType = "";
}

int CollectionInfo::findInPlaylistOrder(const std::string& itemName, const std::vector<std::string>& playlistOrder)
{
    auto it = std::find(playlistOrder.begin(), playlistOrder.end(), itemName);
    if (it != playlistOrder.end())
        return static_cast<int>(std::distance(playlistOrder.begin(), it));
    return INT_MAX;
}

std::unordered_map<std::string, std::vector<std::string>> playlistOrders;
void CollectionInfo::readPlaylistFile(const std::string& playlistName) {
    std::string playlistFilePath = Utils::combinePath(Configuration::absolutePath, "collections", name, "playlists", playlistName + ".txt");
    std::ifstream filestream(playlistFilePath);
    if (!filestream.is_open()) {
        Logger::write(Logger::ZONE_WARNING, "Collection", "Could not open playlist file: " + playlistFilePath);
        return;
    }
    std::string line;
    while (std::getline(filestream, line)) {
        line = Utils::trimEnds(line);
        playlistOrders[playlistName].push_back(line);
    }
    filestream.close();
}

void CollectionInfo::readAllPlaylistFiles() {
    if (!playlistOrders.empty()) {
        return;
    }
    playlistOrders.clear();
    std::string playlistFolderPath = Utils::combinePath(Configuration::absolutePath, "collections", name, "playlists");
    DIR* dir;
    struct dirent* entry;
    dir = opendir(playlistFolderPath.c_str());
    if (dir == nullptr) {
        Logger::write(Logger::ZONE_WARNING, "Collection", "Could not open playlist folder: " + playlistFolderPath);
        return;
    }
    while ((entry = readdir(dir)) != nullptr) {
        std::string fileName = entry->d_name;
        std::string fileExtension = fileName.substr(fileName.find_last_of(".") + 1);
        if (fileExtension == "txt") {
            std::string playlistName = fileName.substr(0, fileName.find_last_of("."));
            Logger::write(Logger::ZONE_INFO, "Collection", "Reading playlist file: " + playlistName);
            readPlaylistFile(playlistName);
        }
    }
    closedir(dir);
}

void CollectionInfo::customSort(std::vector<Item*>& itemsToSort, const std::unordered_map<std::string, std::size_t>& orderIndices, const std::string& mainCollectionName) {
    std::vector<Item*> inPlaylist;
    std::vector<Item*> notInPlaylist;
    for (Item* item : itemsToSort) {
        std::string itemName = item->name;
        if (item->collectionInfo->name != this->name) {
            itemName = "_" + item->collectionInfo->name + ":" + itemName;
        }
        auto it = orderIndices.find(itemName);
        if (it != orderIndices.end()) {
            inPlaylist.push_back(item);
        }
        else {
            notInPlaylist.push_back(item);
        }
    }
    std::sort(inPlaylist.begin(), inPlaylist.end(),
        [&orderIndices, &mainCollectionName](const Item* a, const Item* b) {
            std::string aName = a->name;
            if (a->collectionInfo->name != mainCollectionName) {
                aName = "_" + a->collectionInfo->name + ":" + a->name;
            }
            std::string bName = b->name;
            if (b->collectionInfo->name != mainCollectionName) {
                bName = "_" + b->collectionInfo->name + ":" + b->name;
            }
            std::size_t aIndex = orderIndices.count(aName) > 0 ? orderIndices.at(aName) : std::numeric_limits<std::size_t>::max();
            std::size_t bIndex = orderIndices.count(bName) > 0 ? orderIndices.at(bName) : std::numeric_limits<std::size_t>::max();
            return aIndex < bIndex;
        }
    );
    if (!std::is_sorted(notInPlaylist.begin(), notInPlaylist.end(), [](const Item* a, const Item* b) { return a->name < b->name; })) {
        std::sort(notInPlaylist.begin(), notInPlaylist.end(), [](const Item* a, const Item* b) { return a->name < b->name; });
    }
    inPlaylist.reserve(inPlaylist.size() + notInPlaylist.size());
    std::copy(notInPlaylist.begin(), notInPlaylist.end(), std::back_inserter(inPlaylist));
    itemsToSort.swap(inPlaylist);
}

void CollectionInfo::customSortPlaylist(const std::string& playlistName, std::vector<Item*>* playlist) {

    // Attempt to sort by the attribute specified in playlistName
    if (!playlist->empty() && !playlist->at(0)->getMetaAttribute(playlistName).empty() && playlistName != "lastplayed") {

        // Sorting by custom attribute based on the playlist name
        std::sort(playlist->begin(), playlist->end(), itemIsLess(playlistName));

        return;
    }
    else {
        if (playlistOrders.find(playlistName) == playlistOrders.end()) {
            return;
        }
        const auto& playlistOrder = playlistOrders[playlistName];
        std::unordered_map<std::string, std::size_t> playlistOrderIndices;
        for (std::size_t i = 0; i < playlistOrder.size(); ++i) {
            playlistOrderIndices[playlistOrder[i]] = i;
        }
        customSort(*playlist, playlistOrderIndices, this->name);
    }
}

void CollectionInfo::customSortAllItems() {

    readAllPlaylistFiles();

    for (auto it = playlistOrders.begin(); it != playlistOrders.end(); ++it) {
        std::string playlistName = it->first;
        customSortPlaylist(playlistName, &items); // Assuming items is the vector you want to sort. Adjust as needed.
    }

}
