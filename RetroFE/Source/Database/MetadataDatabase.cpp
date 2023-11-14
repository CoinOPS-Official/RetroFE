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
#include "MetadataDatabase.h"
#include "../Collection/CollectionInfo.h"
#include "../Collection/Item.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "Configuration.h"
#include "DB.h"
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <list>
#include <rapidxml.hpp>
#include <sstream>
#include <string>
#include <map>
#include <sys/types.h>
#include <sqlite3.h>
#include <zlib.h>
#include <exception>

#if defined(__linux) || defined(__APPLE__)
#include <sys/stat.h>
#endif

MetadataDatabase::MetadataDatabase(DB &db, Configuration &c)
    : config_(c)
    , db_(db)
{

}

MetadataDatabase::~MetadataDatabase() = default;

bool MetadataDatabase::resetDatabase()
{
    int rc;
    char *error = nullptr;
    sqlite3 *handle = db_.handle;

    LOG_INFO("Metadata", "Erasing");

    std::string sql;
    sql.append("DROP TABLE IF EXISTS Meta;");

    rc = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &error);

    if(rc != SQLITE_OK)
    {
        std::stringstream ss;
        ss << "Unable to create Metadata table. Error: " << error;
        LOG_ERROR("Metadata", ss.str());
        return false;
    }

    return initialize();
}

bool MetadataDatabase::initialize()
{
    if(needsRefresh())
    {
        int rc;
        char* error = nullptr;
        sqlite3* handle = db_.handle;

        std::string sql;
        sql.append("CREATE TABLE IF NOT EXISTS Meta(");
        sql.append("collectionName TEXT KEY,");
        sql.append("name TEXT NOT NULL DEFAULT '',");
        sql.append("title TEXT NOT NULL DEFAULT '',");
        sql.append("year TEXT NOT NULL DEFAULT '',");
        sql.append("manufacturer TEXT NOT NULL DEFAULT '',");
        sql.append("developer TEXT NOT NULL DEFAULT '',");
        sql.append("genre TEXT NOT NULL DEFAULT '',");
        sql.append("cloneOf TEXT NOT NULL DEFAULT '',");
        sql.append("players TEXT NOT NULL DEFAULT '',");
        sql.append("ctrltype TEXT NOT NULL DEFAULT '',");
        sql.append("buttons TEXT NOT NULL DEFAULT '',");
        sql.append("joyways TEXT NOT NULL DEFAULT '',");
        sql.append("rating TEXT NOT NULL DEFAULT '',");
        sql.append("score TEXT NOT NULL DEFAULT '');");
        sql.append("CREATE UNIQUE INDEX IF NOT EXISTS MetaUniqueId ON Meta(collectionName, name);");

        rc = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &error);

        if (rc != SQLITE_OK)
        {
            std::stringstream ss;
            ss << "Unable to create Metadata table. Error: " << error;
            LOG_ERROR("Metadata", ss.str());

            return false;
        }

        importDirectory();
    }

    return true;
}

bool MetadataDatabase::importDirectory()
{
    DIR *dp;
    struct dirent const *dirp;
    std::string hyperListPath  = Utils::combinePath(Configuration::absolutePath, "meta", "hyperlist");
    std::string mameListPath   = Utils::combinePath(Configuration::absolutePath, "meta", "mamelist");
    std::string emuarcListPath = Utils::combinePath(Configuration::absolutePath, "meta", "emuarc");

    dp = opendir(hyperListPath.c_str());

    if(dp == nullptr)
    {
        LOG_WARNING("MetadataDatabase", "Could not read directory \"" + hyperListPath + "\"");
    }
    else
    {
        while((dirp = readdir(dp)) != nullptr)
        {
            if (dirp->d_type != DT_DIR && std::string(dirp->d_name) != "." && std::string(dirp->d_name) != "..")
            {

                std::string basename = dirp->d_name;

                std::string extension = basename.substr(basename.find_last_of("."), basename.size()-1);
                basename = basename.substr(0, basename.find_last_of("."));
                std::string collectionName = basename.substr(0, basename.find_first_of("."));


                if(extension == ".xml")
                {
                    std::string importFile = Utils::combinePath(hyperListPath, std::string(dirp->d_name));
                    LOG_INFO("Metadata", "Importing hyperlist: " + importFile);
                    importHyperlist(importFile, collectionName);
                }
            }
        }

        if (dp) closedir(dp);
    }

    dp = opendir(mameListPath.c_str());

    if(dp == nullptr)
    {
        LOG_INFO("CollectionInfoBuilder", "Could not read directory \"" + mameListPath + "\"");
    }
    else
    {
        while((dirp = readdir(dp)) != nullptr)
        {
            if (dirp->d_type != DT_DIR && std::string(dirp->d_name) != "." && std::string(dirp->d_name) != "..")
            {

                std::string basename = dirp->d_name;

                std::string extension = basename.substr(basename.find_last_of("."), basename.size()-1);
                basename = basename.substr(0, basename.find_last_of("."));
                std::string collectionName = basename.substr(0, basename.find_first_of("."));


                if(extension == ".xml")
                {
                    std::string importFile = Utils::combinePath(mameListPath, std::string(dirp->d_name));
                    LOG_INFO("Metadata", "Importing mamelist: " + importFile);
                    config_.setProperty("status", "Scraping data from " + importFile);
                    importMamelist(importFile, collectionName);
                }
            }
        }

        if (dp) closedir(dp);
    }

    dp = opendir(emuarcListPath.c_str());

    if(dp == nullptr)
    {
        LOG_INFO("MetadataDatabase", "Could not read directory \"" + emuarcListPath + "\"");
    }
    else
    {
        while((dirp = readdir(dp)) != nullptr)
        {
            if (dirp->d_type != DT_DIR && std::string(dirp->d_name) != "." && std::string(dirp->d_name) != "..")
            {

                std::string basename = dirp->d_name;

                std::string extension = basename.substr(basename.find_last_of("."), basename.size()-1);
                basename = basename.substr(0, basename.find_last_of("."));


                if(extension == ".dat")
                {
                    std::string importFile = Utils::combinePath(emuarcListPath, std::string(dirp->d_name));
                    LOG_INFO("Metadata", "Importing emuarclist: " + importFile);
                    importEmuArclist(importFile);
                }
            }
        }

        if (dp) closedir(dp);
    }

    return true;
}

void MetadataDatabase::injectMetadata(CollectionInfo *collection)
{
    sqlite3 *handle = db_.handle;
    int rc;
    sqlite3_stmt *stmt;

    // items into a hash to make it easily searchable
    std::vector<Item*> const* items = &collection->items;
    std::map<std::string, Item*, std::less<>> itemMap;

    for (auto* item : *items) {
        itemMap.try_emplace(item->name, item);
    }

    //todo: program crashes if this query fails
    sqlite3_prepare_v2(handle,
                       "SELECT DISTINCT Meta.name, Meta.title, Meta.year, Meta.manufacturer, Meta.developer, Meta.genre, Meta.players, Meta.ctrltype, Meta.buttons, Meta.joyways, Meta.cloneOf, Meta.rating, Meta.score "
                       "FROM Meta WHERE collectionName=? ORDER BY title ASC;",
                       -1, &stmt, nullptr);

    sqlite3_bind_text(stmt, 1, collection->metadataType.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    while(rc == SQLITE_ROW)
    {
        std::string name = (const char *)sqlite3_column_text(stmt, 0);
        std::string fullTitle = (const char *)sqlite3_column_text(stmt, 1);
        std::string year = (const char *)sqlite3_column_text(stmt, 2);
        std::string manufacturer = (const char *)sqlite3_column_text(stmt, 3);
        std::string developer = (const char *)sqlite3_column_text(stmt, 4);
        std::string genre = (const char *)sqlite3_column_text(stmt, 5);
        std::string numberPlayers = (const char *)sqlite3_column_text(stmt, 6);
        std::string ctrlType = (const char *)sqlite3_column_text(stmt, 7);
        std::string numberButtons = (const char *)sqlite3_column_text(stmt, 8);
        std::string joyWays = (const char *)sqlite3_column_text(stmt, 9);
        std::string cloneOf = (const char *)sqlite3_column_text(stmt, 10);
        std::string rating = (const char *)sqlite3_column_text(stmt, 11);
        std::string score = (const char *)sqlite3_column_text(stmt, 12);
        std::string launcher;
        std::string title = fullTitle;


        if(std::map<std::string, Item *>::iterator it = itemMap.find(name); it != itemMap.end())
        {
            Item *item = it->second;
            item->title = title;
            item->fullTitle = fullTitle;
            item->year = year;
            item->manufacturer = manufacturer;
            item->developer = developer;
            item->genre = genre;
            item->numberPlayers = numberPlayers;
            item->numberButtons = numberButtons;
            item->ctrlType = ctrlType;
            item->joyWays = joyWays;
            item->cloneof = cloneOf;
            item->rating = rating;
            item->score = score;
        }
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

bool MetadataDatabase::needsRefresh()
{
    sqlite3 *handle = db_.handle;
    sqlite3_stmt *stmt;
    bool result;

    sqlite3_prepare_v2(handle,
                       "SELECT COUNT(*) FROM Meta;",
                       -1, &stmt, nullptr);


    if(int rc = sqlite3_step(stmt); rc == SQLITE_ROW)
    {
        int count = sqlite3_column_int(stmt, 0);
        struct stat metadb;
        struct stat exe;
        int metadbErr  = stat( Utils::combinePath(Configuration::absolutePath, "meta.db").c_str(), &metadb);
#ifdef WIN32
        int exeErr  = stat( Utils::combinePath(Configuration::absolutePath, "retrofe", "RetroFE.exe").c_str(), &exe);
#else
        int exeErr  = stat( Utils::combinePath(Configuration::absolutePath, "RetroFE").c_str(), &exe);
        if (exeErr)
        {
            exeErr  = stat( Utils::combinePath(Configuration::absolutePath, "retrofe").c_str(), &exe);
        }
#endif
        time_t metadirTime = timeDir(Utils::combinePath(Configuration::absolutePath, "meta"));

        result = (count == 0 || metadbErr || metadb.st_mtime < metadirTime || exeErr ) ? true : false;
    }
    else
    {
        result = true;
    }

    sqlite3_finalize(stmt);

    return result;
}

bool MetadataDatabase::importHyperlist(const std::string& hyperlistFile, const std::string& collectionName)
{
    char *error = nullptr;

    config_.setProperty("status", "Scraping data from \"" + hyperlistFile + "\"");
    rapidxml::xml_document<> doc;
    std::ifstream file(hyperlistFile.c_str());
    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try
    {
        buffer.push_back('\0');

        doc.parse<0>(&buffer[0]);

        rapidxml::xml_node<> const *root = doc.first_node("menu");

        if(!root)
        {
            LOG_ERROR("Metadata", "Does not appear to be a HyperList file (missing <menu> tag)");
            return false;
        }
        sqlite3 *handle = db_.handle;
        sqlite3_exec(handle, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &error);
        for(rapidxml::xml_node<> const *game = root->first_node("game"); game; game = game->next_sibling("game"))
        {
            rapidxml::xml_attribute<> const *nameXml = game->first_attribute("name");
            rapidxml::xml_node<> const *descriptionXml = game->first_node("description");
            rapidxml::xml_node<> const *cloneofXml = game->first_node("cloneof");
            rapidxml::xml_node<> const *crcXml = game->first_node("crc");
            rapidxml::xml_node<> const *manufacturerXml = game->first_node("manufacturer");
            rapidxml::xml_node<> const *developerXml = game->first_node("developer");
            rapidxml::xml_node<> const *yearXml = game->first_node("year");
            rapidxml::xml_node<> const *genreXml = game->first_node("genre");
            rapidxml::xml_node<> const *ratingXml = game->first_node("rating");
            rapidxml::xml_node<> const *scoreXml = game->first_node("score");
            rapidxml::xml_node<> const *numberPlayersXml = game->first_node("players");
            rapidxml::xml_node<> const *ctrlTypeXml = game->first_node("ctrltype");
            rapidxml::xml_node<> const *numberButtonsXml = game->first_node("buttons");
            rapidxml::xml_node<> const *numberJoyWaysXml = game->first_node("joyways");
            rapidxml::xml_node<> const *enabledXml = game->first_node("enabled");
            std::string name = nameXml ? nameXml->value() : "";
            std::string description = descriptionXml ? descriptionXml->value() : "";
            std::string crc = crcXml ? crcXml->value() : "";
            std::string cloneOf = cloneofXml ? cloneofXml->value() : "";
            std::string manufacturer = manufacturerXml ? manufacturerXml->value() : "";
            std::string developer = developerXml ? developerXml->value() : "";
            std::string year = yearXml ? yearXml->value() : "";
            std::string genre = genreXml ? genreXml->value() : "";
            std::string rating = ratingXml ? ratingXml->value() : "";
            std::string score = scoreXml ? scoreXml->value() : "";
            std::string numberPlayers = numberPlayersXml ? numberPlayersXml->value() : "";
            std::string ctrlType = ctrlTypeXml ? ctrlTypeXml->value() : "";
            std::string numberButtons = numberButtonsXml ? numberButtonsXml->value() : "";
            std::string numberJoyWays = numberJoyWaysXml ? numberJoyWaysXml->value() : "";
            std::string enabled = enabledXml ? enabledXml->value() : "";

            if(name.length() > 0)
            {
                sqlite3_stmt *stmt;

                sqlite3_prepare_v2(handle,
                                   "INSERT OR REPLACE INTO Meta (name, title, year, manufacturer, developer, genre, players, ctrltype, buttons, joyways, cloneOf, collectionName, rating, score) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                                   -1, &stmt, nullptr);

                sqlite3_bind_text(stmt,  1, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  2, description.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  3, year.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  4, manufacturer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  5, developer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  6, genre.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  7, numberPlayers.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  8, ctrlType.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  9, numberButtons.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 10, numberJoyWays.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 11, cloneOf.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 12, collectionName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 13, rating.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 14, score.c_str(), -1, SQLITE_TRANSIENT);

                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
        config_.setProperty("status", "Saving data from \"" + hyperlistFile + "\" to database");
        sqlite3_exec(handle, "COMMIT TRANSACTION;", nullptr, nullptr, &error);

        return true;
    }
    catch(rapidxml::parse_error &e)
    {
        std::string what = e.what();
        auto line = static_cast<long>(std::count(&buffer.front(), e.where<char>(), char('\n')) + 1);
        std::stringstream ss;
        ss << "Could not parse layout file. [Line: " << line << "] Reason: " << e.what();

        LOG_ERROR("Metadata", ss.str());
    }
    catch(std::exception &e)
    {
        std::string what = e.what();
        LOG_ERROR("Metadata", "Could not parse hyperlist file. Reason: " + what);
    }


    return false;
}

bool MetadataDatabase::importMamelist(const std::string& filename, const std::string& collectionName)
{
    rapidxml::xml_document<> doc;
    rapidxml::xml_node<> const * rootNode;
    char *error = nullptr;
    sqlite3 *handle = db_.handle;

    config_.setProperty("status", "Scraping data from \"" + filename + "\" (this will take a while)");

    LOG_INFO("Mamelist", "Importing mamelist file \"" + filename + "\" (this will take a while)");
    std::ifstream file(filename.c_str());

    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    buffer.push_back('\0');

    doc.parse<0>(&buffer[0]);

    rootNode = doc.first_node("mame");

    if(!rootNode)
    {
        LOG_ERROR("Metadata", "Does not appear to be a MameList file (missing <mame> tag)");
        return false;
    }

    if(sqlite3_exec(handle, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &error) != SQLITE_OK)
    {
        std::string emsg = error;
        LOG_ERROR("Metadata", "SQL Error starting transaction: " + emsg);
        return false;
    };
    std::string gameNodeName = "game";

    // support new mame formats
    if(rootNode->first_node(gameNodeName.c_str()) == nullptr) {
        gameNodeName = "machine";
    }

    for (rapidxml::xml_node<> const * game = rootNode->first_node(gameNodeName.c_str()); game; game = game->next_sibling())
    {
        rapidxml::xml_attribute<> const *nameNode = game->first_attribute("name");
        rapidxml::xml_attribute<> const *cloneOfXml = game->first_attribute("cloneof");

        if(nameNode != nullptr)
        {
            std::string name = nameNode->value();
            rapidxml::xml_node<> const *descriptionNode = game->first_node("description");
            rapidxml::xml_node<> const *yearNode = game->first_node("year");
            rapidxml::xml_node<> const *manufacturerNode = game->first_node("manufacturer");
            rapidxml::xml_node<> const *genreNode = game->first_node("genre");
            rapidxml::xml_node<> const *inputNode = game->first_node("input");

            std::string description = (descriptionNode == nullptr) ? nameNode->value() : descriptionNode->value();
            std::string year = (yearNode == nullptr) ? "" : yearNode->value();
            std::string manufacturer = (manufacturerNode == nullptr) ? "" : manufacturerNode->value();
            std::string genre = (genreNode == nullptr) ? "" : genreNode->value();
            std::string cloneOf = (cloneOfXml == nullptr) ? "" : cloneOfXml->value();
            std::string players;
            std::string buttons;

            if(inputNode != nullptr)
            {
                rapidxml::xml_attribute<> const *playersAttribute = inputNode->first_attribute("players");
                rapidxml::xml_attribute<> const *buttonsAttribute = inputNode->first_attribute("buttons");

                if(playersAttribute)
                {
                    players = playersAttribute->value();
                }

                if(buttonsAttribute)
                {
                    buttons = buttonsAttribute->value();
                }

            }

            sqlite3_stmt *stmt;

            sqlite3_prepare_v2(handle,
                               "INSERT OR REPLACE INTO Meta (name, title, year, manufacturer, genre, players, buttons, cloneOf, collectionName) VALUES (?,?,?,?,?,?,?,?,?)",
                               -1, &stmt, nullptr);


            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, year.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, manufacturer.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, genre.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, players.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, buttons.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, cloneOf.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, collectionName.c_str(), -1, SQLITE_TRANSIENT);

            if (int code = sqlite3_step(stmt) != SQLITE_DONE)
            {
                std::stringstream ss;
                ss << "Failed to insert machine \"" << name << "\" into database; " << sqlite3_errstr(code) << "; " << sqlite3_errmsg(handle);
                LOG_ERROR("Metadata", ss.str());
                sqlite3_finalize(stmt);
                break;
            };
            sqlite3_finalize(stmt);
        }
    }

    config_.setProperty("status", "Saving data from \"" + filename + "\" to database");
    if (sqlite3_exec(handle, "COMMIT TRANSACTION;", nullptr, nullptr, &error) != SQLITE_OK)
    {
        std::string emsg = error;
        LOG_ERROR("Metadata", "SQL Error closing transaction: " + emsg);
    };

    return true;
}


bool MetadataDatabase::importEmuArclist(const std::string& emuarclistFile)
{
    char *error = nullptr;

    config_.setProperty("status", "Scraping data from \"" + emuarclistFile + "\"");
    rapidxml::xml_document<> doc;
    std::ifstream file(emuarclistFile.c_str());
    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try
    {
        buffer.push_back('\0');

        doc.parse<0>(&buffer[0]);

        rapidxml::xml_node<> const *root = doc.first_node("datafile");

        if(!root)
        {
            LOG_ERROR("Metadata", "Does not appear to be a EmuArcList file (missing <datafile> tag)");
            return false;
        }

        rapidxml::xml_node<> const *header = root->first_node("header");
        if (!header)
        {
            LOG_ERROR("Metadata", "Does not appear to be a EmuArcList file (missing <header> tag)");
            return false;
        }
        rapidxml::xml_node<> const *name = header->first_node("name");
        if (!name)
        {
            LOG_ERROR("Metadata", "Does not appear to be a EmuArcList SuperDat file (missing <name> in <header> tag)");
            return false;
        }
        std::string collectionName = name->value();
        if(std::size_t pos = collectionName.find(" - "); pos != std::string::npos)
        {
            collectionName = collectionName.substr(0, pos);
        }
        sqlite3 *handle = db_.handle;
        sqlite3_exec(handle, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &error);
        

        for(rapidxml::xml_node<> const *game = root->first_node("game"); game; game = game->next_sibling("game"))
        {
            rapidxml::xml_node<> const *descriptionXml = game->first_node("description");
            rapidxml::xml_node<> const *emuarcXml      = game->first_node("EmuArc");
            if (!emuarcXml)
            {
                LOG_ERROR("Metadata", "Does not appear to be a EmuArcList SuperDat file (missing <emuarc> tag)");
                return false;
            }
            rapidxml::xml_node<> const *cloneofXml       = emuarcXml->first_node("cloneof");
            rapidxml::xml_node<> const *manufacturerXml  = emuarcXml->first_node("publisher");
            rapidxml::xml_node<> const *developerXml     = emuarcXml->first_node("developer");
            rapidxml::xml_node<> const *yearXml          = emuarcXml->first_node("year");
            rapidxml::xml_node<> const *genreXml         = emuarcXml->first_node("genre");
            rapidxml::xml_node<> const *subgenreXml      = emuarcXml->first_node("subgenre");
            rapidxml::xml_node<> const *ratingXml        = emuarcXml->first_node("ratings");
            rapidxml::xml_node<> const *scoreXml         = emuarcXml->first_node("score");
            rapidxml::xml_node<> const *numberPlayersXml = emuarcXml->first_node("players");
            rapidxml::xml_node<> const *enabledXml       = emuarcXml->first_node("enabled");
            std::string name          = descriptionXml ? descriptionXml->value() : "";
            std::string description   = descriptionXml ? descriptionXml->value() : "";
            std::string crc           = "";
            std::string cloneOf       = cloneofXml ? cloneofXml->value() : "";
            std::string manufacturer  = manufacturerXml ? manufacturerXml->value() : "";
            std::string developer     = developerXml ? developerXml->value() : "";
            std::string year          = yearXml ? yearXml->value() : "";
            std::string genre         = genreXml ? genreXml->value() : "";
            genre                     = (subgenreXml && subgenreXml->value_size() != 0) ? genre + "_" + subgenreXml->value() : genre;
            std::string rating        = ratingXml ? ratingXml->value() : "";
            std::string score         = scoreXml ? scoreXml->value() : "";
            std::string numberPlayers = numberPlayersXml ? numberPlayersXml->value() : "";
            std::string ctrlType      = "";
            std::string numberButtons = "";
            std::string numberJoyWays = "";
            std::string enabled       = enabledXml ? enabledXml->value() : "";

            if(name.length() > 0)
            {
                sqlite3_stmt *stmt;

                sqlite3_prepare_v2(handle,
                                   "INSERT OR REPLACE INTO Meta (name, title, year, manufacturer, developer, genre, players, ctrltype, buttons, joyways, cloneOf, collectionName, rating, score) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                                   -1, &stmt, nullptr);

                sqlite3_bind_text(stmt,  1, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  2, description.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  3, year.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  4, manufacturer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  5, developer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  6, genre.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  7, numberPlayers.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  8, ctrlType.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,  9, numberButtons.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 10, numberJoyWays.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 11, cloneOf.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 12, collectionName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 13, rating.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 14, score.c_str(), -1, SQLITE_TRANSIENT);

                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
        config_.setProperty("status", "Saving data from \"" + emuarclistFile + "\" to database");
        sqlite3_exec(handle, "COMMIT TRANSACTION;", nullptr, nullptr, &error);

        return true;
    }
    catch(rapidxml::parse_error &e)
    {
        std::string what = e.what();
        auto line = static_cast<long>(std::count(&buffer.front(), e.where<char>(), char('\n')) + 1);
        std::stringstream ss;
        ss << "Could not parse layout file. [Line: " << line << "] Reason: " << e.what();

        LOG_ERROR("Metadata", ss.str());
    }
    catch(std::exception &e)
    {
        std::string what = e.what();
        LOG_ERROR("Metadata", "Could not parse EmuArclist file. Reason: " + what);
    }


    return false;
}


time_t MetadataDatabase::timeDir(const std::string& path )
{
    time_t lastTime = 0;
    DIR *dp;
    struct dirent const *dirp;

    dp = opendir( path.c_str( ) );

    while (dp != nullptr && (dirp = readdir( dp )) != nullptr)
    {
        std::string file = dirp->d_name;

        // Check if file is a directory
        struct stat sb;
        if (file != "." && file != ".." && stat( Utils::combinePath( path, file ).c_str( ), &sb ) == 0 && S_ISDIR( sb.st_mode ))
        {
            time_t tmpTime = timeDir( Utils::combinePath( path, file ) );
            lastTime = (tmpTime > lastTime) ? tmpTime : lastTime;
        }
        else if (file != "." && file != "..")
        {
            struct stat filestat;
            int err = stat( Utils::combinePath( path, file ).c_str( ), &filestat );
            lastTime = (!err && filestat.st_mtime > lastTime) ? filestat.st_mtime : lastTime;
        }
    }

    if (dp) closedir( dp );

    return lastTime;
}
