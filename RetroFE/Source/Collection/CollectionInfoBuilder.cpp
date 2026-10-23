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
#include "ItemSort.h"
#include "../Database/Configuration.h"
#include "../Database/MetadataDatabase.h"
#include "../Database/DB.h"
#include "../Database/GlobalOpts.h"
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
#include <unordered_map>
#include <unordered_set>

CollectionInfoBuilder::CollectionInfoBuilder(Configuration &c, MetadataDatabase &mdb)
    : conf_(c)
    , metaDB_(mdb)
{
}

namespace fs = std::filesystem;

namespace {
std::map<std::string, std::string, std::less<>> importPlaylistTitles(const fs::path& path) {
    std::map<std::string, std::string, std::less<>> titles;
    std::ifstream input(path / "titles.conf");
    auto trim = [](const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string()
            : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    size_t lineNumber = 0;
    for (std::string line; std::getline(input, line);) {
        if (++lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            LOG_WARNING("Collection", "Ignoring invalid playlist title at " +
                (path / "titles.conf").string() + ":" + std::to_string(lineNumber));
            continue;
        }
        std::string id = trim(line.substr(0, separator));
        std::string title = trim(line.substr(separator + 1));
        if (id.size() >= 4 && id.compare(id.size() - 4, 4, ".txt") == 0)
            id.resize(id.size() - 4);
        if (id.empty() || title.empty() || id.find_first_of("/\\") != std::string::npos)
            continue;
        titles.insert_or_assign(std::move(id), std::move(title));
    }
    return titles;
}

bool importListNames(const std::string& file, std::unordered_set<std::string>& names,
    std::vector<std::string>* ordered = nullptr) {
    std::ifstream input(file);
    if (!input.good()) return false;
    for (std::string line; std::getline(input, line);) {
        line = Utils::filterComments(line);
        if (!line.empty() && names.insert(line).second && ordered) ordered->push_back(line);
    }
    return true;
}
}

CollectionInfoBuilder::~CollectionInfoBuilder() = default;

bool CollectionInfoBuilder::createCollectionDirectory(const std::string& name, const std::string& collectionType, const std::string& osType)
{
    std::string collectionPath = Utils::combinePath(Configuration::absolutePath, "collections", name);
    std::string collectionFolderPath = Utils::combinePath(Configuration::absolutePath, "collections");
    
    // Check if the collections folder doesn't exist
    if (!fs::exists(collectionFolderPath) || !fs::is_directory(collectionFolderPath)) {
        std::cout << std::endl;
        std::cout << "Collections folder not found in \"" << Configuration::absolutePath << "\"" << std::endl;
        std::cout << "Creating collections folder in \"" << Configuration::absolutePath << "/collections" << "\"" << std::endl;
        try {
            fs::create_directory("collections");
            }
        catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error creating folder: " << e.what() << std::endl;
            }
    }
    else {
        std::cout << "Collections folder found in \"" << Configuration::absolutePath << "\"" << std::endl;
    }
    std::cout << std::endl;
    
    // Check if a collection called <name> doesn't exist
    if (!fs::exists(collectionPath)) {
        // Create the collection called <name>
        std::cout << "Creating a collection called " << name << " at " << "\"" << collectionPath << "\"" << std::endl;
        
        std::vector<std::string> paths;
        paths.push_back(collectionPath);
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "artwork_back"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "artwork_front"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "bezel"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "logo"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "marquee"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "medium_back"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "medium_front"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "screenshot"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "screentitle"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "video"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "videoPlayer"));
        paths.push_back(Utils::combinePath(collectionPath, "medium_artwork", "videoSD"));
        paths.push_back(Utils::combinePath(collectionPath, "roms"));
        paths.push_back(Utils::combinePath(collectionPath, "system_artwork"));
        paths.push_back(Utils::combinePath(collectionPath, "launchers"));
        
        // Folders for local launchers
        if (collectionType == "local") {
            paths.push_back(Utils::combinePath(collectionPath, "launchers." + osType + ".local"));
        }

        for(auto it = paths.begin(); it != paths.end(); it++) {
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
        if (collectionType == " ") {
            settingsFile << "launcher = mame" << std::endl;
        }
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
        
        if (collectionType == "local") {
            filename = Utils::combinePath(collectionPath, "launcher." + osType + ".conf");
            std::cout << "Creating file \"" << filename << "\"" << std::endl;
            std::ofstream localFile;
            localFile.open(filename.c_str());
            localFile.close();
        }
        
//        filename = Utils::combinePath(collectionPath, "menu.txt");
//        std::cout << "Creating file \"" << filename << "\"" << std::endl;
//        std::ofstream menuFile;
//        menuFile.open(filename.c_str());
//        menuFile.close();
    }
    else {
        std::cout << "A collection called " << name << " at " << "\"" << collectionPath << "\"" << " already exists" << std::endl;
    }

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

    if (!conf_.getProperty(launcherKey, launcherName)) {
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

    if (!includeStream.good()) {
        return false;
    }

    std::string line; 
    Item* i;
    while(std::getline(includeStream, line)) {
        line = Utils::filterComments(line);
        if (!line.empty() && list.find(line) == list.end()) {
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

    if (!includeStream.good()) {
        return false;
    }

    std::unordered_set<std::string> names;
    names.reserve(list.size());
    for (const auto* item : list) names.insert(item->name);
    std::string line;
    Item* i;
    while(std::getline(includeStream, line)) {
        line = Utils::filterComments(line);
        if (!line.empty()) {
            if (names.insert(line).second) {
                i = new Item();
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

bool CollectionInfoBuilder::ImportDirectory(CollectionInfo* info,
    const std::string& mergedCollectionName) {
    std::string path = info->listpath;
    std::vector<std::string> includeFilterUnsorted;
    std::unordered_set<std::string> includeFilter;
    std::unordered_set<std::string> excludeFilter;

    std::string includeFile = Utils::combinePath(
        Configuration::absolutePath, "collections", info->name, "include.txt");
    std::string excludeFile = Utils::combinePath(
        Configuration::absolutePath, "collections", info->name, "exclude.txt");

    bool showMissing = false;
    bool romHierarchy = false;
    bool emuarc = false;

    bool mergedFileUsed = false;
    bool mergedFileHadEntries = false;

    // --- Sub / merged-collection handling ---
    if (!mergedCollectionName.empty())
    {
        // Legacy-style: collections/<Master>/<Sub>.sub
        std::string legacyFile = Utils::combinePath(
            Configuration::absolutePath, "collections",
            mergedCollectionName,            // master (e.g. Arcade)
            info->name + ".sub"              // sub    (e.g. RetroPC)
        );

        // Doc-style: collections/<Sub>/<Master>.sub
        std::string docStyleFile = Utils::combinePath(
            Configuration::absolutePath, "collections",
            info->name,                      // sub    (e.g. Cave)
            mergedCollectionName + ".sub"    // master (e.g. Arcade)
        );

        std::string mergedFile;

        // Prefer legacy if present, else doc-style
        if (fs::exists(legacyFile)) {
            LOG_INFO("CollectionInfoBuilder",
                "Using legacy sub file \"" + legacyFile +
                "\" for merged collection \"" + info->name +
                "\" (master: \"" + mergedCollectionName + "\")");
            mergedFile = legacyFile;
        }
        else if (fs::exists(docStyleFile)) {
            LOG_INFO("CollectionInfoBuilder",
                "Using doc-style sub file \"" + docStyleFile +
                "\" for merged collection \"" + info->name +
                "\" (master: \"" + mergedCollectionName + "\")");
            mergedFile = docStyleFile;
        }

        if (!mergedFile.empty())
        {
            mergedFileUsed = true;

            // inherit includeMissingItems from the MASTER first
            conf_.getProperty(
                "collections." + mergedCollectionName + ".list.includeMissingItems",
                showMissing);

            // Track how many entries we had before reading the .sub
            const auto before = includeFilterUnsorted.size();

            importListNames(mergedFile, includeFilter, &includeFilterUnsorted);

            if (includeFilterUnsorted.size() > before) {
                mergedFileHadEntries = true; // non-empty .sub
            }
        }
    }

    // Sub/own collection can still override includeMissingItems, romHierarchy, emuarc
    conf_.getProperty("collections." + info->name + ".list.includeMissingItems",
        showMissing);
    conf_.getProperty("collections." + info->name + ".list.romHierarchy",
        romHierarchy);
    conf_.getProperty("collections." + info->name + ".list.emuarc",
        emuarc);
    if (emuarc)
        romHierarchy = true;

    LOG_INFO("CollectionInfoBuilder", "Checking for \"" + includeFile + "\"");

    // IMPORTANT:
    // Only import include.txt from THIS collection if:
    //  - we are not in merged/sub mode, OR
    //  - we are in merged/sub mode but the .sub was empty (full merge)
    if (!mergedFileUsed || !mergedFileHadEntries)
    {
        importListNames(includeFile, includeFilter, &includeFilterUnsorted);
    }

    importListNames(excludeFile, excludeFilter);

    // If showMissing==true, names in includeFilterUnsorted become items directly
    if (showMissing) {
        for (const auto& name : includeFilterUnsorted) {
            if (excludeFilter.contains(name)) continue;
            auto* item = new Item();
            item->name = item->title = item->fullTitle = name;
            item->collectionInfo = info;
            info->items.push_back(item);
        }
    }
    includeFilterUnsorted.clear();

    // Read ROM directory if:
    //  - showMissing is false (normal rom-dir driven), OR
    //  - includeFilter is empty (no include.txt/sub filter)
    if (!showMissing || includeFilter.empty()) {
        std::unordered_set<std::string> existingNames;
        existingNames.reserve(info->items.size());
        for (const auto* item : info->items) existingNames.insert(item->name);
        do {
            std::string rompath;
            if (size_t position = path.find(';');
                position != std::string::npos) {
                rompath = path.substr(0, position);
                path = path.substr(position + 1);
            }
            else {
                rompath = path;
                path.clear();
            }

            ImportRomDirectory(rompath, info,
                includeFilter, excludeFilter,
                existingNames, romHierarchy, emuarc);
        } while (!path.empty());
    }

    // --- Time spent + play count handling (unchanged from your newer version) ---

    std::string timeSpentFile = Utils::combinePath(
        Configuration::absolutePath, "collections", "timeSpent.txt");
    std::map<std::string, double> currentTimeSpentList =
        ImportTimeSpent(timeSpentFile);

    for (auto* item : info->items) {
        std::string key = getKey(item);
        auto it = currentTimeSpentList.find(key);
        if (it != currentTimeSpentList.end()) {
            item->timeSpent = it->second;
        }
    }

    std::string playCountFile = Utils::combinePath(
        Configuration::absolutePath, "collections", "playCount.txt");
    std::map<std::string, Item*> currentPlayCountList =
        ImportPlayCount(playCountFile);

    if (!currentPlayCountList.empty()) {
        for (auto* item : info->items) {
            std::string key = "_" + info->name + ":" + item->name;
            Item* src = nullptr;

            if (auto found = currentPlayCountList.find(key); found != currentPlayCountList.end()) {
                src = found->second;
            }
            if (!src) {
                if (auto found = currentPlayCountList.find(item->name); found != currentPlayCountList.end())
                    src = found->second;
            }

            if (src) {
                item->playCount = src->playCount;
                item->lastPlayed = src->lastPlayed;
            }
        }
    }

    // cleanup lists
    for (auto& pair : currentPlayCountList) {
        delete pair.second;
    }
    currentPlayCountList.clear();

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
    if ( !excludeAllFilter.empty()) {
        info->playlists["all"] = new std::vector<Item *>();
        for(auto it = info->items.begin(); it != info->items.end(); it++) {
            bool found = false;
            for(auto itex = excludeAllFilter.begin(); itex != excludeAllFilter.end(); itex++) {
                collectionName = info->name;
                itemName       = itex->first;
                if (itemName.at(0) == '_') // name consists of _<collectionName>:<itemName>
                {
                     itemName.erase(0, 1); // Remove _
                     size_t position = itemName.find(":");
                     if (position != std::string::npos ) {
                         collectionName = itemName.substr(0, position);
                         itemName       = itemName.erase(0, position+1);
                     }
                }
                if ( ((*it)->name == itemName || itemName == "*") && (*it)->collectionInfo->name == collectionName ) {
                    found = true;
                }
            }
            if ( !found ) {
                info->playlists["all"]->push_back((*it));
            }
        }
        for (auto& pair : excludeAllFilter) {
            delete pair.second;
        }
        excludeAllFilter.clear();
    }
    else {
        info->playlists["all"] = &info->items;
    }

    // lookup playlist location and add playlists and what items they have
    std::map<std::string, Item*> playlistItems;
    std::string path = Utils::combinePath(Configuration::absolutePath, "collections", info->name, "playlists");
    loadPlaylistItems(info, &playlistItems, path);

    // find and load favorites from global playlist if enabled
    bool globalFavLast = false;
    (void)conf_.getProperty(OPTION_GLOBALFAVLAST, globalFavLast);
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
    conf_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
    conf_.getProperty(OPTION_CYCLEPLAYLIST, cycleString);
    // use the global setting as overide if firstCollection == current
    if (cycleString == "" || firstCollection != info->name) {
        // check if collection has different setting
        if (conf_.propertyExists(settingPrefix + OPTION_CYCLEPLAYLIST)) {
            conf_.getProperty(settingPrefix + OPTION_CYCLEPLAYLIST, cycleString);
        }
    }
    if (info->name == "Favorites") {
        cycleString = "favorites";
    }

    std::vector<std::string> cycleVector;
    Utils::listToVector(cycleString, cycleVector, ',');

    if (cycleVector.size()) {
        // add in order according to cycle list
        for (auto itP = cycleVector.begin(); itP != cycleVector.end(); itP++) {
            if (playlistItems[*itP]) {
                info->playlistItems.push_back(playlistItems[*itP]);
            }
        }
    }
    else {
        // convert lookup playlist map to vector
        for (auto itP = playlistItems.begin(); itP != playlistItems.end(); itP++) {
            info->playlistItems.push_back(itP->second);
        }
    }

    // intialize empty dynamic playlists
    if(info->playlists["favorites"] == nullptr) {
        info->playlists["favorites"] = new std::vector<Item *>();
    }

    if(info->playlists["lastplayed"] == nullptr) {
        info->playlists["lastplayed"] = new std::vector<Item *>();
    }

    return;
}

void CollectionInfoBuilder::loadPlaylistItems(CollectionInfo* info, std::map<std::string, Item*>* playlistItems, const std::string& path)
{
    info->playlistTitles = importPlaylistTitles(path);

    // Get playlist cycle
    std::string settingPrefix = "collections." + info->name + ".";
    std::string cycleString;
    std::string firstCollection = "";
    conf_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
    conf_.getProperty(OPTION_CYCLEPLAYLIST, cycleString);

    if (cycleString.empty() || firstCollection != info->name) {
        if (conf_.propertyExists(settingPrefix + OPTION_CYCLEPLAYLIST)) {
            conf_.getProperty(settingPrefix + OPTION_CYCLEPLAYLIST, cycleString);
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

    // Preserve the first matching item and collection order for wildcard entries.
    std::unordered_map<std::string, std::unordered_map<std::string, Item*>> byName;
    std::unordered_map<std::string, std::vector<Item*>> byCollection;
    for (auto* item : info->items) {
        const auto& collectionName = item->collectionInfo->name;
        byName[collectionName].try_emplace(item->name, item);
        byCollection[collectionName].push_back(item);
    }
    for (const auto& entry : fs::directory_iterator(path)) {
        if (fs::is_regular_file(entry)) {
            std::string file = entry.path().filename().string();

            if (file == "." || file == "..")
                continue;

            size_t position = file.find_last_of(".");
            std::string basename = (position == std::string::npos) ? file : file.substr(0, position);

            std::string comparator = ".txt";
            size_t start = file.length() >= comparator.length() ? file.length() - comparator.length() : std::string::npos;

            if (start != std::string::npos && file.compare(start, comparator.length(), comparator) == 0) {
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
                playlistItem->title = info->getPlaylistTitle(basename);
                playlistItem->fullTitle = playlistItem->title;
                playlistItem->leaf = false;
                playlistItem->collectionInfo = info;
                std::string sortType = Item::validSortType(basename) ? basename : "";

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

                    auto append = [&](Item* item) {
                            if (pfItem->playCount) {
                                item->playCount = pfItem->playCount;
                                item->lastPlayed = pfItem->lastPlayed;
                            }
                            if (basename == "favorites")
                                item->isFavorite = true;
                            info->playlists[basename]->push_back(item);

                    };
                    if (itemName == "*") {
                        if (auto found = byCollection.find(collectionName); found != byCollection.end())
                            for (auto* item : found->second) append(item);
                    }
                    else if (auto collection = byName.find(collectionName); collection != byName.end()) {
                        if (auto found = collection->second.find(itemName); found != collection->second.end())
                            append(found->second);
                    }
                }
                if (info->playlists[basename]->size()) {
                    playlistItems->insert({ basename, playlistItem });
                }
                else {
                    delete playlistItem; // Delete unused playlist item
                }

                // Destroy the dummy filter items!
                for (Item* dummy : playlistFilter) {
                    delete dummy;
                }
                playlistFilter.clear();
            }
        }
    }
}
void CollectionInfoBuilder::updateLastPlayedPlaylist(CollectionInfo* info, Item* item, int size) {
    // --- Step 1: Basic Setup and Playlist Logic (Unchanged) ---

    std::string playlistCollectionName = info->name;
    bool globalFavLast = false;
    conf_.getProperty(OPTION_GLOBALFAVLAST, globalFavLast);
    if (globalFavLast) {
        playlistCollectionName = "Favorites";
    }

    if (size == 0) {
        return;
    }

    // Define paths using your Utils::combinePath utility.
    // The results are strings, as per your function's return type.
    std::string playlistsPathStr = Utils::combinePath(Configuration::absolutePath, "collections", playlistCollectionName, "playlists");
    std::string playlistFileStr = Utils::combinePath(playlistsPathStr, "lastplayed.txt");

    // Load the existing list of last played items.
    std::vector<Item*> lastplayedList;
    ImportBasicList(info, playlistFileStr, lastplayedList);

    if (info->playlists["lastplayed"] == nullptr) {
        info->playlists["lastplayed"] = new std::vector<Item*>();
    }
    info->playlists["lastplayed"]->clear();

    item->playCount++;
    item->lastPlayed = std::to_string(std::time(nullptr));

    info->playlists["lastplayed"]->push_back(item);

    // Persist global entries even when their collection is not loaded here.
    // The active playlist can only contain pointers resolved in this collection.
    auto savedName = [&playlistCollectionName](const Item* entry) {
        return entry->collectionInfo->name == playlistCollectionName
            ? entry->name : "_" + entry->collectionInfo->name + ":" + entry->name;
    };
    std::vector<std::string> savedEntries{savedName(item)};
    for (Item* oldItem : lastplayedList) {
        if (savedEntries.size() >= static_cast<unsigned int>(size)) {
            break;
        }
        std::string collectionName = info->name;
        std::string itemName = oldItem->name;
        bool qualified = false;
        if (!itemName.empty() && itemName.front() == '_') {
            const auto colon = itemName.find(':');
            if (colon != std::string::npos && colon > 1 && colon + 1 < itemName.size()) {
                collectionName = itemName.substr(1, colon - 1);
                itemName.erase(0, colon + 1);
                qualified = true;
            }
        }
        if (collectionName == item->collectionInfo->name && itemName == item->name) continue;
        bool resolved = false;
        for (Item* mainListItem : info->items) {
            if (mainListItem->name == itemName && mainListItem->collectionInfo->name == collectionName && mainListItem != item) {
                info->playlists["lastplayed"]->push_back(mainListItem);
                savedEntries.push_back(savedName(mainListItem));
                resolved = true;
                break;
            }
        }
        if (!resolved && globalFavLast && qualified && collectionName != info->name) {
            savedEntries.push_back(oldItem->name);
        }
    }

    for (Item* dummy : lastplayedList) delete dummy;
    lastplayedList.clear();

    // --- Step 2: Filesystem Operations using the String Paths ---

    LOG_INFO("Collection", "Saving last played playlist to: " + playlistFileStr);

    try {
        // fs::create_directories is overloaded to accept a string path directly.
        fs::create_directories(playlistsPathStr);

        // std::ofstream is also happy to accept a string path.
        std::ofstream filestream(playlistFileStr);
        if (!filestream) {
            LOG_ERROR("Collection", "Save failed: Could not open " + playlistFileStr + " for writing.");
            return;
        }

        for (const auto& entry : savedEntries) filestream << entry << std::endl;
        filestream.close();

    }
    catch (const fs::filesystem_error& e) {
        LOG_ERROR("Collection", "Filesystem error while saving last played list: " + std::string(e.what()));
        return;
    }

    // --- Step 3: Configurable Sorting
    std::string lastPlayedSortMode = "time";
    conf_.getProperty(OPTION_LASTPLAYEDSORTTYPE, lastPlayedSortMode);
    std::string sortType = (lastPlayedSortMode == "time") ? "lastplayed" : "";
    sortItemsByKeys(*info->playlists["lastplayed"], sortType, true, true);

    AddToPlayCount(item);

}

std::string CollectionInfoBuilder::getKey(Item* item)
{
    return "_" + item->collectionInfo->name + ":" + item->name;
}

void CollectionInfoBuilder::updateTimeSpent(Item* item, double timePlayedInSeconds) {
    if (!item || timePlayedInSeconds <= 0) {
        LOG_WARNING("CollectionInfoBuilder", "Invalid item or gameplay duration; cannot update time spent.");
        return;
    }

    // Update the current item's timeSpent attribute in memory
    item->timeSpent += timePlayedInSeconds;

    std::string dir = Utils::combinePath(Configuration::absolutePath, "collections");
    std::string file = Utils::combinePath(dir, "timeSpent.txt");

    // Load existing timeSpent data
    std::map<std::string, double> currentTimes = ImportTimeSpent(file);

    // Update or add the current item's time spent in the map
    std::string key = getKey(item);
    currentTimes[key] = item->timeSpent;

    // Persist updated timeSpent data to file
    std::ofstream outstream(file, std::ios::trunc);
    if (!outstream.is_open()) {
        LOG_ERROR("CollectionInfoBuilder", "Failed to open " + file + " for writing.");
        return;
    }

    for (const auto& [key, timeSpent] : currentTimes) {
        outstream << key << ";" << timeSpent << std::endl;
    }

    outstream.close();
    LOG_INFO("CollectionInfoBuilder", "Updated timeSpent.txt for " + item->name + ": +" + std::to_string(timePlayedInSeconds) + " seconds, total: " + std::to_string(item->timeSpent) + " seconds.");
}

void CollectionInfoBuilder::AddToPlayCount(Item* item) {
    // Write new playcount file
    std::string dir = Utils::combinePath(Configuration::absolutePath, "collections");
    std::string file = Utils::combinePath(Configuration::absolutePath, "collections", "playCount.txt");

    std::map<std::string, Item*> curretPlayCountList = ImportPlayCount(file);

    // THE FIX 1: Safely delete the dummy item before overwriting it
    auto it = curretPlayCountList.find(getKey(item));
    if (it != curretPlayCountList.end()) {
        delete it->second;
    }
    curretPlayCountList[getKey(item)] = item;
    LOG_INFO("PlayCount", "Saving " + item->name + " " + std::to_string(item->playCount));

    LOG_INFO("PlayCount", "Saving " + file);

    std::ofstream filestream;
    try {
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
        else if (!(infostat.st_mode & S_IFDIR)) {
            LOG_WARNING("PlayCount", dir + " exists, but is not a directory.");
            return;
        }

        // write file
        filestream.open(file.c_str());
        for (auto it = curretPlayCountList.begin(); it != curretPlayCountList.end(); it++) {
            // append play count and last played time
            filestream << it->first << ";" << it->second->playCount << ";" << it->second->lastPlayed << std::endl;
        }

        filestream.close();
    }
    catch (std::exception&) {
        LOG_ERROR("PlayCount", "Save failed: " + file);
    }

    // Clean up all dummy items, leaving the real one alone
    for (auto& pair : curretPlayCountList) {
        if (pair.second != item) {
            delete pair.second;
        }
    }
    curretPlayCountList.clear();
}

std::map<std::string, double> CollectionInfoBuilder::ImportTimeSpent(const std::string& file) {
    std::map<std::string, double> timeSpentList;

    std::ifstream filestream(file);
    if (!filestream.is_open()) {
        return timeSpentList;
    }

    std::string line;
    while (std::getline(filestream, line)) {
        line = Utils::filterComments(line);
        if (!line.empty()) {
            size_t separator = line.find(';');
            if (separator != std::string::npos) {
                std::string key = line.substr(0, separator);
                double timeSpent = std::stod(line.substr(separator + 1));
                timeSpentList[key] = timeSpent;
            }
        }
    }
    filestream.close();

    return timeSpentList;
}


std::map<std::string, Item*> CollectionInfoBuilder::ImportPlayCount(const std::string& file)
{
    std::ifstream includeStream(file.c_str());
    std::map<std::string, Item*> curretPlayCountList;
    if (!includeStream.good()) {
        return curretPlayCountList;
    }

    // parse play count and last played time
    std::string extra;
    size_t extraPosition;
    size_t timePosition;
   
    std::string line;
    while (std::getline(includeStream, line)) {
        line = Utils::filterComments(line);
        if (!line.empty()) {
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            extraPosition = line.find(";");
            if (extraPosition != std::string::npos) {
                extra = line.substr(extraPosition + 1);
                timePosition = extra.find(";");
                line = line.substr(0, extraPosition);
                if (timePosition != std::string::npos) {
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

void CollectionInfoBuilder::ImportRomDirectory(const std::string& path, CollectionInfo* info,
    const std::unordered_set<std::string>& includeFilter,
    const std::unordered_set<std::string>& excludeFilter,
    std::unordered_set<std::string>& existingNames, bool romHierarchy, bool emuarc)
{
    std::vector<std::string> extensions;
    info->extensionList(extensions);

    LOG_INFO("CollectionInfoBuilder", "Scanning directory \"" + path + "\"");
    if (!fs::exists(path) || !fs::is_directory(path))
    {
        LOG_INFO("CollectionInfoBuilder", "Could not read directory \"" + path + "\". Ignore if this is a menu.");
        return;
    }

    for (const auto& entry : fs::directory_iterator(path)) {
        std::string file = entry.path().filename().string();

        // Check if the file is a directory or a file
        if (romHierarchy && fs::is_directory(entry) && file != "." && file != "..") {
            ImportRomDirectory(entry.path().string(), info, includeFilter, excludeFilter, existingNames, romHierarchy, emuarc);
        }
        else if (fs::is_regular_file(entry) && file != "." && file != "..") {
            size_t position = file.find_last_of(".");
            std::string basename = (std::string::npos == position) ? file : file.substr(0, position);

            // if there is an include list, only include roms that are found and are in the include list
            // if there is an exclude list, exclude those roms
            if ((includeFilter.empty() || includeFilter.find(basename) != includeFilter.end()) &&
                (excludeFilter.empty() || excludeFilter.find(basename) == excludeFilter.end()))
            {
                // iterate through all known file extensions
                for (const std::string& ext : extensions) {
                    std::string comparator = "." + ext;
                    size_t start = file.length() >= comparator.length() ? file.length() - comparator.length() : std::string::npos;

                    if (start != std::string::npos && file.compare(start, comparator.length(), comparator) == 0) {
                        // Add item if it doesn't already exist
                        if (!existingNames.contains(basename)) {
                            auto* i = new Item();

                            i->name = basename;
                            i->fullTitle = basename;
                            i->title = basename;
                            i->collectionInfo = info;
                            i->filepath = path + Utils::pathSeparator;

                            if (emuarc) {
                                i->file = basename;
                                i->name = Utils::getFileName(path);
                                i->fullTitle = i->name;
                                i->title = i->name;
                            }
                            info->items.push_back(i);
                            existingNames.insert(i->name);
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
