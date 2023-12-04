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
#include "CollectionInfoBuilder.h"
#include "CollectionInfo.h"
#include "Item.h"
#include "../Database/Configuration.h"
#include "../Database/MetadataDatabase.h"
#include "../Database/DB.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"

#if defined(__linux) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <cstring>
#endif

#include <sstream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <filesystem>

CollectionInfoBuilder::CollectionInfoBuilder(Configuration &c, MetadataDatabase &mdb)
    : conf_(c)
    , metaDB_(mdb)
{
}

namespace fs = std::filesystem;

CollectionInfoBuilder::~CollectionInfoBuilder() = default;

bool CollectionInfoBuilder::createCollectionDirectory(const std::string& name)
{
    std::string collectionPath = Utils::combinePath(Configuration::absolutePath, "collections", name);

    std::vector<std::string> paths;
    paths.push_back(collectionPath);
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "artwork_back"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "artwork_front"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "bezel"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "logo"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "medium_back"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "medium_front"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "screenshot"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "screentitle"));
    paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "video"));
    paths.push_back(Utils::combinePath(collectionPath, "roms"));
    paths.push_back(Utils::combinePath(collectionPath, "system_artwork"));

    for(auto it = paths.begin(); it != paths.end(); it++)
    {
        std::cout << "Creating folder \"" << *it << "\"" << std::endl;

#if defined(_WIN32) && !defined(__GNUC__)
        if (!CreateDirectory(it->c_str(), nullptr))
        {
            if (ERROR_ALREADY_EXISTS != GetLastError())
            {
                std::cout << "Could not create folder \"" << *it << "\"" << std::endl;
                return false;
            }
        }
#else 
#if defined(__MINGW32__)
        if (mkdir(it->c_str()) == -1)
#else
        if (mkdir(it->c_str(), 0755) == -1)
#endif        
        {
           std::cout << "Could not create folder \"" << *it << "\":" << errno << std::endl;
        }
    #endif
    }

    std::string filename = Utils::combinePath(collectionPath, "include.txt");
    std::cout << "Creating file \"" << filename << "\"" << std::endl;

    std::ofstream includeFile;
    includeFile.open(filename.c_str());
    includeFile << "# Add a list of files to show on the menu (one filename per line, without the extension)." << std::endl;
    includeFile << "# If no items are in this list then all files in the folder specified" << std::endl;
    includeFile << "# by settings.conf will be used" << std::endl;
    includeFile.close();

    filename = Utils::combinePath(collectionPath, "exclude.txt");
    std::cout << "Creating file \"" << filename << "\"" << std::endl;
    std::ofstream excludeFile;
    excludeFile.open(filename.c_str());

    includeFile << "# Add a list of files to hide on the menu (one filename per line, without the extension)." << std::endl;
    excludeFile.close();

    filename = Utils::combinePath(collectionPath, "settings.conf");
    std::cout << "Creating file \"" << filename << "\"" << std::endl;
    std::ofstream settingsFile;
    settingsFile.open(filename.c_str());

    settingsFile << "# Uncomment and edit the following line to use a different ROM path." << std::endl;
    settingsFile << "#list.path = " << Utils::combinePath("%BASE_ITEM_PATH%", "%ITEM_COLLECTION_NAME%", "roms") << std::endl;
    settingsFile << "list.includeMissingItems = false" << std::endl;
    settingsFile << "list.extensions = zip" << std::endl;
    settingsFile << "list.menuSort = yes" << std::endl;
    settingsFile << std::endl;
    settingsFile << "launcher = mame" << std::endl;
    settingsFile << "#metadata.type = MAME" << std::endl;
    settingsFile << std::endl;
    settingsFile << std::endl;
    settingsFile << "#media.screenshot      = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "screenshot") << std::endl;
    settingsFile << "#media.screentitle     = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "screentitle") << std::endl;
    settingsFile << "#media.artwork_back    = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "artwork_back") << std::endl;
    settingsFile << "#media.artwork_front   = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "artwork_front") << std::endl;
    settingsFile << "#media.logo            = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "logo") << std::endl;
    settingsFile << "#media.medium_back     = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "medium_back") << std::endl;
    settingsFile << "#media.medium_front    = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "medium_front") << std::endl;
    settingsFile << "#media.screenshot      = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "screenshot") << std::endl;
    settingsFile << "#media.screentitle     = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "screentitle") << std::endl;
    settingsFile << "#media.video           = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "medium_artwork", "video") << std::endl;
    settingsFile << "#media.system_artwork  = " << Utils::combinePath("%BASE_MEDIA_PATH%", "%ITEM_COLLECTION_NAME%", "system_artwork") << std::endl;
    settingsFile.close();

    filename = Utils::combinePath(collectionPath, "menu.txt");
    std::cout << "Creating file \"" << filename << "\"" << std::endl;
    std::ofstream menuFile;
    menuFile.open(filename.c_str());
    menuFile.close();

    return true;
}
CollectionInfo *CollectionInfoBuilder::buildCollection(const std::string& name)
{
   return buildCollection(name, "");
}

CollectionInfo *CollectionInfoBuilder::buildCollection(const std::string& name, const std::string& mergedCollectionName)
{
    std::string listItemsPathKey = "collections." + name + ".list.path";
    std::string listFilterKey = "collections." + name + ".list.filter";
    std::string extensionsKey = "collections." + name + ".list.extensions";
    std::string launcherKey = "collections." + name + ".launcher";

    //todo: metadata is not fully not implemented
    std::string metadataTypeKey = "collections." + name + ".metadata.type";
    std::string metadataPathKey = "collections." + name + ".metadata.path";

    std::string listItemsPath;
    std::string launcherName;
    std::string extensions;
    std::string metadataType = name;
    std::string metadataPath;
    
    conf_.getCollectionAbsolutePath(name, listItemsPath);

    (void)conf_.getProperty(extensionsKey, extensions);
    (void)conf_.getProperty(metadataTypeKey, metadataType);
    (void)conf_.getProperty(metadataPathKey, metadataPath);

    if (!conf_.getProperty(launcherKey, launcherName))
    {
        std::stringstream ss;
        ss        << "\""
                  << launcherKey
                  << "\" points to a launcher that is not configured (launchers."
                  << launcherName
                  << "). Your collection will be viewable, however you will not be able to "
                  << "launch any of the items in your collection.";

        LOG_NOTICE("Collections", ss.str());
    }

    auto *collection = new CollectionInfo(conf_, name, listItemsPath, extensions, metadataType, metadataPath);

    (void)conf_.getProperty("collections." + collection->name + ".launcher", collection->launcher);

    ImportDirectory(collection, mergedCollectionName);

    return collection;
}


bool CollectionInfoBuilder::ImportBasicList(CollectionInfo *info, const std::string& file, std::map<std::string, Item *> &list)
{
    std::ifstream includeStream(file.c_str());

    if (!includeStream.good())
    {
        return false;
    }

    std::string line; 
    Item* i;
    while(std::getline(includeStream, line))
    {
        line = Utils::filterComments(line);
        
        if (!line.empty() && list.find(line) == list.end())
        {
            i = new Item();
            line.erase( std::remove(line.begin(), line.end(), '\r'), line.end() );

            i->fullTitle = line;
            i->name = line;
            i->title = line;
            i->collectionInfo = info;

            list[line] = i;
        }
    }

    return true;
}

bool CollectionInfoBuilder::ImportBasicList(CollectionInfo *info, const std::string& file, std::vector<Item *> &list)
{
    std::ifstream includeStream(file.c_str());

    if (!includeStream.good())
    {
        return false;
    }

    std::string line; 
    Item* i;
    while(std::getline(includeStream, line))
    {
        line = Utils::filterComments(line);
        
        if (!line.empty())
        {

            bool found = false;
            for (auto it = list.begin(); it != list.end(); ++it)
            {
                if (line == (*it)->name)
                {
                    found = true;
                }
            }

            if (!found)
            {
                i = new Item();
                line.erase( std::remove(line.begin(), line.end(), '\r'), line.end() );

                i->fullTitle = line;
                i->name = line;
                i->title = line;
                i->collectionInfo = info;

                list.push_back(i);
            }

        }
    }

    return true;
}

bool CollectionInfoBuilder::ImportDirectory(CollectionInfo *info, const std::string& mergedCollectionName)
{
    std::string path = info->listpath;
    std::vector<Item *> includeFilterUnsorted;
    std::map<std::string, Item *> includeFilter;
    std::map<std::string, Item *> excludeFilter;
    std::string includeFile    = Utils::combinePath(Configuration::absolutePath, "collections", info->name, "include.txt");
    std::string excludeFile    = Utils::combinePath(Configuration::absolutePath, "collections", info->name, "exclude.txt");

    std::string launcher;
    bool showMissing  = false; 
    bool romHierarchy = false;
    bool emuarc       = false;
 
    if (mergedCollectionName != "")
    {
        
        std::string mergedFile = Utils::combinePath(Configuration::absolutePath, "collections", mergedCollectionName, info->name + ".sub");
        LOG_INFO("CollectionInfoBuilder", "Checking for \"" + mergedFile + "\"");
        (void)conf_.getProperty("collections." + mergedCollectionName + ".list.includeMissingItems", showMissing);
        ImportBasicList(info, mergedFile, includeFilterUnsorted);
        ImportBasicList(info, mergedFile, includeFilter);

    }
    (void)conf_.getProperty("collections." + info->name + ".list.includeMissingItems", showMissing);
    (void)conf_.getProperty("collections." + info->name + ".list.romHierarchy", romHierarchy);
    (void)conf_.getProperty("collections." + info->name + ".list.emuarc", emuarc);
    if (emuarc)
        romHierarchy = true;

    LOG_INFO("CollectionInfoBuilder", "Checking for \"" + includeFile + "\"");
    ImportBasicList(info, includeFile, includeFilterUnsorted);
    ImportBasicList(info, includeFile, includeFilter);
    ImportBasicList(info, excludeFile, excludeFilter);

    for(auto it = includeFilterUnsorted.begin(); it != includeFilterUnsorted.end(); ++it)
    {
        if (showMissing && excludeFilter.find((*it)->name) == excludeFilter.end())
        {
            info->items.push_back(*it);
        }
        else
        {
            delete *it;
        }
    }
    includeFilterUnsorted.clear( );

    // Read ROM directory if showMissing is false
    if (!showMissing || includeFilter.empty())
    {
        do
        {
             std::string rompath;
             if(size_t position = path.find( ";" ); position != std::string::npos)
             {
                 rompath = path.substr(0, position);
                 path    = path.substr(position+1);
             }
             else
             {
                 rompath = path;
                 path    = "";
             }
             ImportRomDirectory(rompath, info, includeFilter, excludeFilter, romHierarchy, emuarc);
        } while (path != "");
    }

    // apply playCount data
    std::string playCountFile = Utils::combinePath(Configuration::absolutePath, "collections", "playCount.txt");
    std::map<std::string, Item*> curretPlayCountList = ImportPlayCount(playCountFile);
    std::string lookup;
    Item* i = nullptr;

    if (!curretPlayCountList.empty()) {
        for (auto it = info->items.begin(); it != info->items.end(); ++it)
        {   
            lookup = "_" + info->name + ":" + (*it)->name;
            if (curretPlayCountList[lookup]) {
                i = curretPlayCountList[lookup];
            }
            else if (curretPlayCountList[(*it)->name]) {
                i = curretPlayCountList[(*it)->name];
            }
            if (i != nullptr) {
                (*it)->playCount = i->playCount;
                (*it)->lastPlayed = i->lastPlayed;
            }
            i = nullptr;
        }
    }

    // cleanup lists
    while(!includeFilter.empty())
    {
        auto it = includeFilter.begin();
        // delete the unused items if they were never pushed to the main collection
        if (!showMissing)
        {
            delete it->second;
        }
        includeFilter.erase(it);
    }
    while(!excludeFilter.empty())
    {
        auto it = excludeFilter.begin();
        delete it->second;
        excludeFilter.erase(it);
    }
    while (!curretPlayCountList.empty())
    {
        auto it = curretPlayCountList.begin();
        delete it->second;
        curretPlayCountList.erase(it);
    }

    return true;
}


void CollectionInfoBuilder::addPlaylists(CollectionInfo *info)
{
    std::string itemName;
    std::string collectionName;
    std::map<std::string, Item *> excludeAllFilter;
    std::string excludeAllFile = Utils::combinePath(Configuration::absolutePath, "collections", info->name, "exclude_all.txt");

    ImportBasicList(info, excludeAllFile, excludeAllFilter);
    // adds items to "all" list except those found in "exclude_all.txt"
    if ( !excludeAllFilter.empty())
    {
        info->playlists["all"] = new std::vector<Item *>();
        for(auto it = info->items.begin(); it != info->items.end(); it++)
        {
            bool found = false;
            for(auto itex = excludeAllFilter.begin(); itex != excludeAllFilter.end(); itex++)
            {
                collectionName = info->name;
                itemName       = itex->first;
                if (itemName.at(0) == '_') // name consists of _<collectionName>:<itemName>
                {
                     itemName.erase(0, 1); // Remove _
                     size_t position = itemName.find(":");
                     if (position != std::string::npos )
                     {
                         collectionName = itemName.substr(0, position);
                         itemName       = itemName.erase(0, position+1);
                     }
                }
                if ( ((*it)->name == itemName || itemName == "*") && (*it)->collectionInfo->name == collectionName )
                {
                    found = true;
                }
            }
            if ( !found )
            {
                info->playlists["all"]->push_back((*it));
            }
        }
        while(!excludeAllFilter.empty())
        {
            auto it = excludeAllFilter.begin();
            excludeAllFilter.erase(it);
        }
    }
    else
    {
        info->playlists["all"] = &info->items;
    }

    // lookup playlist location and add playlists and what items they have
    std::map<std::string, Item*> playlistItems;
    std::string path = Utils::combinePath(Configuration::absolutePath, "collections", info->name, "playlists");
    loadPlaylistItems(info, &playlistItems, path);

    // find and load favorites from global playlist if enabled
    bool globalFavLast = false;
    (void)conf_.getProperty("globalFavLast", globalFavLast);
    if (globalFavLast) {
        std::map<std::string, Item*> tmpPlaylistItems;
        // don't use collection's playlist
        if (info->playlists["favorites"]) {
            info->playlists["favorites"]->clear();
            std::string path = Utils::combinePath(Configuration::absolutePath, "collections", "Favorites", "playlists");
            loadPlaylistItems(info, &tmpPlaylistItems, path);
        }
        else {
            info->playlists["favorites"] = new std::vector<Item*>();
        }
    }

    // no playlists found, done
    if (!playlistItems.size()) {
        return;
    }

    // if cyclePlaylist then order playlist menu items by that
    // get playlist cycle
    std::string settingPrefix = "collections." + info->name + ".";
    std::string cycleString;
    std::string firstCollection = "";
    conf_.getProperty("firstCollection", firstCollection);
    conf_.getProperty("cyclePlaylist", cycleString);
    // use the global setting as overide if firstCollection == current
    if (cycleString == "" || firstCollection != info->name) {
        // check if collection has different setting
        if (conf_.propertyExists(settingPrefix + "cyclePlaylist")) {
            conf_.getProperty(settingPrefix + "cyclePlaylist", cycleString);
        }
    }
    if (info->name == "Favorites") {
        cycleString = "favorites";
    }

    std::vector<std::string> cycleVector;
    Utils::listToVector(cycleString, cycleVector, ',');

    if (cycleVector.size())
    {
        // add in order according to cycle list
        for (auto itP = cycleVector.begin(); itP != cycleVector.end(); itP++)
        {
            if (playlistItems[*itP]) {
                info->playlistItems.push_back(playlistItems[*itP]);
            }
        }
    }
    else {
        // convert lookup playlist map to vector
        for (auto itP = playlistItems.begin(); itP != playlistItems.end(); itP++)
        {
            info->playlistItems.push_back(itP->second);
        }
    }

    // intialize empty dynamic playlists
    if(info->playlists["favorites"] == nullptr)
    {
        info->playlists["favorites"] = new std::vector<Item *>();
    }

    if(info->playlists["lastplayed"] == nullptr)
    {
        info->playlists["lastplayed"] = new std::vector<Item *>();
    }

    return;
}

void CollectionInfoBuilder::loadPlaylistItems(CollectionInfo* info, std::map<std::string, Item*>* playlistItems, const std::string& path)
{
    // Get playlist cycle
    std::string settingPrefix = "collections." + info->name + ".";
    std::string cycleString;
    std::string firstCollection = "";
    conf_.getProperty("firstCollection", firstCollection);
    conf_.getProperty("cyclePlaylist", cycleString);

    if (cycleString.empty() || firstCollection != info->name) {
        if (conf_.propertyExists(settingPrefix + "cyclePlaylist")) {
            conf_.getProperty(settingPrefix + "cyclePlaylist", cycleString);
        }
    }
    if (info->name == "Favorites") {
        cycleString = "favorites";
    }

    std::vector<std::string> cycleVector;
    Utils::listToVector(cycleString, cycleVector, ',');

    if (!fs::exists(path) || !fs::is_directory(path)) {
        info->playlists["favorites"] = new std::vector<Item*>();
        return;
    }

    for (const auto& entry : fs::directory_iterator(path)) {
        if (fs::is_regular_file(entry)) {
            std::string file = entry.path().filename().string();

            if (file == "." || file == "..")
                continue;

            size_t position = file.find_last_of(".");
            std::string basename = (position == std::string::npos) ? file : file.substr(0, position);

            std::string comparator = ".txt";
            size_t start = file.length() - comparator.length();

            if (file.compare(start, comparator.length(), comparator) == 0) {
                // Check if basename is in cycleVector
                if (!cycleVector.empty() && std::find(cycleVector.begin(), cycleVector.end(), basename) == cycleVector.end()) {
                    LOG_INFO("RetroFE", "Skipping playlist: " + basename + ", not in cyclePlaylist");
                    continue;
                }

                LOG_INFO("RetroFE", "Loading playlist: " + basename);

                std::vector<Item*> playlistFilter;
                std::string playlistFile = Utils::combinePath(path, file);
                ImportBasicList(info, playlistFile, playlistFilter);

                info->playlists[basename] = new std::vector<Item*>();

                auto* playlistItem = new Item();
                playlistItem->name = basename;
                playlistItem->title = basename;
                playlistItem->fullTitle = basename;
                playlistItem->leaf = false;
                playlistItem->collectionInfo = info;
                playlistItems->insert({ basename, playlistItem });

                for (Item const* pfItem : playlistFilter) {
                    std::string collectionName = info->name;
                    std::string itemName = pfItem->name;
                    if (itemName.at(0) == '_') {
                        itemName.erase(0, 1);
                        size_t colonPos = itemName.find(":");
                        if (colonPos != std::string::npos) {
                            collectionName = itemName.substr(0, colonPos);
                            itemName = itemName.erase(0, colonPos + 1);
                        }
                    }

                    for (Item* item : info->items) {
                        if ((item->name == itemName || itemName == "*") && item->collectionInfo->name == collectionName) {
                            if (pfItem->playCount) {
                                item->playCount = pfItem->playCount;
                                item->lastPlayed = pfItem->lastPlayed;
                            }
                            if (basename == "favorites")
                                item->isFavorite = true;
                            info->playlists[basename]->push_back(item);

                            if (itemName != "*")
                                break;
                        }
                    }
                }
                playlistFilter.clear();
            }
        }
    }
    // Function ends here
}
void CollectionInfoBuilder::updateLastPlayedPlaylist(CollectionInfo *info, Item *item, int size)
{
    std::string playlistCollectionName = info->name;
    bool globalFavLast = false;
    (void)conf_.getProperty("globalFavLast", globalFavLast);
    if (globalFavLast) {
        playlistCollectionName = "Favorites";
    }
    std::string path = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists");
    LOG_INFO("RetroFE", "Updating lastplayed playlist");

    std::vector<Item *> lastplayedList;
    std::string playlistFile = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists", "lastplayed.txt");
    ImportBasicList(info, playlistFile, lastplayedList);

    if (info->playlists["lastplayed"] == nullptr)
        info->playlists["lastplayed"] = new std::vector<Item *>();
    else
        info->playlists["lastplayed"]->clear();

    if (size == 0)
        return;
    // update the curren't items play count and last played time to be used for meta info/sorting and writing back to list
    item->playCount++;
#ifdef _WIN32
    item->lastPlayed = std::to_string(std::time(0));
#else
    item->lastPlayed = std::to_string(time(0));
#endif

    // Put the new item at the front of the list.
    info->playlists["lastplayed"]->push_back(item);

    // Add the items already in the playlist up to the lastplayedSize.
    for(auto it = lastplayedList.begin(); it != lastplayedList.end(); it++)
    {
        if (info->playlists["lastplayed"]->size() >= static_cast<unsigned int>( size ))
            break;

        std::string collectionName = info->name;
        std::string itemName       = (*it)->name;
        if (itemName.at(0) == '_') // name consists of _<collectionName>:<itemName>
        {
             itemName.erase(0, 1); // Remove _
             size_t position = itemName.find(":");
             if (position != std::string::npos )
             {
                 collectionName = itemName.substr(0, position);
                 itemName       = itemName.erase(0, position+1);
             }
        }

        for(auto it = info->items.begin(); it != info->items.end(); it++)
        {
            if ( (*it)->name == itemName && (*it)->collectionInfo->name == collectionName && (*it) != item)
            {
                info->playlists["lastplayed"]->push_back((*it));
            }
        }
    }

    // Write new lastplayed playlist
    std::string dir  = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists");
    std::string file = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists", "lastplayed.txt");
    LOG_INFO("Collection", "Saving " + file);

    std::ofstream filestream;
    try
    {
        // Create playlists directory if it does not exist yet.
        if (struct stat infostat;  stat( dir.c_str(), &infostat ) != 0 )
        {
#if defined(_WIN32) && !defined(__GNUC__)
            if(!CreateDirectory(dir.c_str(), NULL))
            {
                if(ERROR_ALREADY_EXISTS != GetLastError())
                {
                    LOG_WARNING("Collection", "Could not create directory " + dir);
                    return;
                }
            }
#else 
#if defined(__MINGW32__)
            if(mkdir(dir.c_str()) == -1)
#else
            if(mkdir(dir.c_str(), 0755) == -1)
#endif        
            {
                LOG_WARNING("Collection", "Could not create directory " + dir);
                return;
            }
#endif
        }
        else if ( !(infostat.st_mode & S_IFDIR) )
        {
            LOG_WARNING("Collection", dir + " exists, but is not a directory.");
            return;
        }

        // write playlist file
        filestream.open(file.c_str());
        std::vector<Item *> *saveitems = info->playlists["lastplayed"];
        for(auto it = saveitems->begin(); it != saveitems->end(); it++)
        { 
            // append play count and last played time
            if ((*it)->collectionInfo->name == playlistCollectionName)
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
        LOG_ERROR("Collection", "Save failed: " + file);
    }

    // sort last played by play time with empty values last
    std::string sortType = "lastplayed";
    std::sort(info->playlists["lastplayed"]->begin(), info->playlists["lastplayed"]->end(), [&sortType](Item const* lhs, Item const* rhs) {

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
        });

    AddToPlayCount(item);

    return;
}

std::string CollectionInfoBuilder::getKey(Item* item)
{
    return "_" + item->collectionInfo->name + ":" + item->name;
}

void CollectionInfoBuilder::AddToPlayCount(Item* item)
{
    // Write new playcount file
    std::string dir = Utils::combinePath(Configuration::absolutePath, "collections");
    std::string file = Utils::combinePath(Configuration::absolutePath, "collections", "playCount.txt");

    // todo read to playcount
    std::map<std::string, Item*> curretPlayCountList = ImportPlayCount(file);    
    curretPlayCountList[getKey(item)] = item;
    LOG_INFO("PlayCount", "Saving " + item->name + " " + std::to_string(item->playCount));

    LOG_INFO("PlayCount", "Saving " + file);

    std::ofstream filestream;
    try
    {
        // Create playlists directory if it does not exist yet.
        if (struct stat infostat; stat(dir.c_str(), &infostat) != 0)
        {
#if defined(_WIN32) && !defined(__GNUC__)
            if (!CreateDirectory(dir.c_str(), NULL))
            {
                if (ERROR_ALREADY_EXISTS != GetLastError())
                {
                    LOG_WARNING("PlayCount", "Could not create directory " + dir);
                    return;
                }
            }
#else 
#if defined(__MINGW32__)
            if (mkdir(dir.c_str()) == -1)
#else
            if (mkdir(dir.c_str(), 0755) == -1)
#endif        
            {
                LOG_WARNING("PlayCount", "Could not create directory " + dir);
                return;
            }
#endif
        }
        else if (!(infostat.st_mode & S_IFDIR))
        {
            LOG_WARNING("PlayCount", dir + " exists, but is not a directory.");
            return;
        }

        // write file
        filestream.open(file.c_str());
        for (auto it = curretPlayCountList.begin(); it != curretPlayCountList.end(); it++)
        {
            // append play count and last played time
            filestream << it->first << ";" << it->second->playCount << ";" << it->second->lastPlayed << std::endl;
        }

        filestream.close();
    }
    catch (std::exception&)
    {
        LOG_ERROR("PlayCount", "Save failed: " + file);
    }
}

std::map<std::string, Item*> CollectionInfoBuilder::ImportPlayCount(const std::string& file)
{
    std::ifstream includeStream(file.c_str());
    std::map<std::string, Item*> curretPlayCountList;
    if (!includeStream.good())
    {
        return curretPlayCountList;
    }

    // parse play count and last played time
    std::string extra;
    size_t extraPosition;
    size_t timePosition;
   
    std::string line;
    while (std::getline(includeStream, line))
    {
        line = Utils::filterComments(line);
        if (!line.empty())
        {
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            extraPosition = line.find(";");
            if (extraPosition != std::string::npos)
            {
                extra = line.substr(extraPosition + 1);
                timePosition = extra.find(";");
                line = line.substr(0, extraPosition);
                if (timePosition != std::string::npos)
                {
                    auto* item = new Item();
                    item->lastPlayed = extra.substr(timePosition + 1);
                    item->playCount = Utils::convertInt(extra.substr(0, timePosition));
                    curretPlayCountList.insert({ line, item });
                }
            }
        }
    }

    return curretPlayCountList;
}

void CollectionInfoBuilder::ImportRomDirectory(const std::string& path, CollectionInfo* info, std::map<std::string, Item*> includeFilter, std::map<std::string, Item*> excludeFilter, bool romHierarchy, bool emuarc)
{
    std::vector<std::string> extensions;
    info->extensionList(extensions);

    LOG_INFO("CollectionInfoBuilder", "Scanning directory \"" + path + "\"");
    if (!fs::exists(path) || !fs::is_directory(path))
    {
        LOG_INFO("CollectionInfoBuilder", "Could not read directory \"" + path + "\". Ignore if this is a menu.");
        return;
    }

    for (const auto& entry : fs::directory_iterator(path))
    {
        std::string file = entry.path().filename().string();

        // Check if the file is a directory or a file
        if (romHierarchy && fs::is_directory(entry) && file != "." && file != "..")
        {
            ImportRomDirectory(entry.path().string(), info, includeFilter, excludeFilter, romHierarchy, emuarc);
        }
        else if (fs::is_regular_file(entry) && file != "." && file != "..")
        {
            size_t position = file.find_last_of(".");
            std::string basename = (std::string::npos == position) ? file : file.substr(0, position);

            // if there is an include list, only include roms that are found and are in the include list
            // if there is an exclude list, exclude those roms
            if ((includeFilter.empty() || includeFilter.find(basename) != includeFilter.end()) &&
                (excludeFilter.empty() || excludeFilter.find(basename) == excludeFilter.end()))
            {
                // iterate through all known file extensions
                for (const std::string& ext : extensions)
                {
                    std::string comparator = "." + ext;
                    size_t start = file.length() >= comparator.length() ? file.length() - comparator.length() : std::string::npos;

                    if (start != std::string::npos && file.compare(start, comparator.length(), comparator) == 0)
                    {
                        // Add item if it doesn't already exist
                        bool found = std::any_of(info->items.begin(), info->items.end(), [&](const Item* item) { return item->name == basename; });
                        if (!found) {
                            auto* i = new Item();

                            i->name = basename;
                            i->fullTitle = basename;
                            i->title = basename;
                            i->collectionInfo = info;
                            i->filepath = path + Utils::pathSeparator;

                            if (emuarc)
                            {
                                i->file = basename;
                                i->name = Utils::getFileName(path);
                                i->fullTitle = i->name;
                                i->title = i->name;
                            }
                            info->items.push_back(i);
                        }
                    }
                }
            }
        }
    }
}


void CollectionInfoBuilder::injectMetadata(CollectionInfo *info)
{
    metaDB_.injectMetadata(info);
    return;
}