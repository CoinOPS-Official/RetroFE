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
#include <filesystem>
#include <functional>


namespace fs = std::filesystem;

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
    std::string hyperListPath = Utils::combinePath(Configuration::absolutePath, "meta", "hyperlist");
    std::string mameListPath = Utils::combinePath(Configuration::absolutePath, "meta", "mamelist");
    std::string emuarcListPath = Utils::combinePath(Configuration::absolutePath, "meta", "emuarc");

    // Function to process directory
    auto processDirectory = [&](const std::string& path, const std::string& extension,
        const std::function<void(const std::string&, const std::string&)>& importFunc) {
            if (!fs::exists(path) || !fs::is_directory(path)) {
                LOG_WARNING("MetadataDatabase", "Could not read directory \"" + path + "\"");
                return;
            }

            for (const auto& entry : fs::directory_iterator(path)) {
                if (fs::is_regular_file(entry) && entry.path().extension() == extension) {
                    std::string importFile = entry.path().string();
                    std::string basename = entry.path().stem().string();
                    std::string collectionName = basename.substr(0, basename.find_first_of("."));

                    LOG_INFO("Metadata", "Importing " + extension.substr(1) + ": " + importFile);
                    importFunc(importFile, collectionName);
                }
            }
        };

    // Process each directory with corresponding extension and import function
    processDirectory(hyperListPath, ".xml", [this](const std::string& file, const std::string& name) { importHyperlist(file, name); });
    processDirectory(mameListPath, ".xml", [this](const std::string& file, const std::string& name) { importMamelist(file, name); });
    processDirectory(emuarcListPath, ".dat", [this](const std::string& file, const std::string& name) { importEmuArclist(file); });

    return true;
}

void MetadataDatabase::injectMetadata(CollectionInfo* collection)
{
    sqlite3* handle = db_.handle;
    sqlite3_stmt* stmt;

    std::vector<Item*> const* items = &collection->items;
    std::map<std::string, Item*, std::less<>> itemMap;

    for (auto* item : *items) {
        itemMap.try_emplace(item->name, item);
    }

    // Prepare SQL query - ensure this query is optimized and uses appropriate indexing
    const char* sql = "SELECT DISTINCT Meta.name, Meta.title, Meta.year, Meta.manufacturer, Meta.developer, Meta.genre, Meta.players, Meta.ctrltype, Meta.buttons, Meta.joyways, Meta.cloneOf, Meta.rating, Meta.score "
        "FROM Meta WHERE collectionName=? ORDER BY title ASC;";

    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::stringstream ss;
        ss << "Error in injectMetadata: Failed to prepare SQL statement. SQL Error: " << sqlite3_errmsg(handle);
        LOG_ERROR("Metadata", ss.str());
        return;
    }

    sqlite3_bind_text(stmt, 1, collection->metadataType.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        // Directly use the C-style strings for comparison and assignment to avoid unnecessary conversions
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        if (auto it = itemMap.find(name); it != itemMap.end())
        {
            Item* item = it->second;

            // Directly assign the C-style strings from the SQLite columns to the corresponding fields in the Item object.
            // reinterpret_cast is used to convert the void pointer returned by sqlite3_column_text to a const char pointer.
            item->title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            item->year = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            item->manufacturer = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            item->developer = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            item->genre = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            item->numberPlayers = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            item->ctrlType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            item->numberButtons = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            item->joyWays = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            item->cloneof = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
            item->rating = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            item->score = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        }
    }

    // Check for errors in the while loop, log and handle appropriately
    if (sqlite3_errcode(handle) != SQLITE_DONE) {
        std::stringstream ss;
        ss << "Error in injectMetadata: SQLite operation did not complete successfully. Error code: " << sqlite3_errcode(handle) << "; Error message: " << sqlite3_errmsg(handle);
        LOG_ERROR("Metadata", ss.str());
    }

    sqlite3_finalize(stmt);
}

bool MetadataDatabase::needsRefresh()
{
    bool metaLock = false;
    config_.getProperty("metaLock", metaLock);
    if (metaLock)
        return false;

    sqlite3* handle = db_.handle;
    sqlite3_stmt* stmt;
    bool result;

    sqlite3_prepare_v2(handle, "SELECT COUNT(*) FROM Meta;", -1, &stmt, nullptr);

    if (int rc = sqlite3_step(stmt); rc == SQLITE_ROW)
    {
        int count = sqlite3_column_int(stmt, 0);

        fs::path metaDbPath = Utils::combinePath(Configuration::absolutePath, "meta.db");
        fs::path exePath;

#ifdef WIN32
        exePath = Utils::combinePath(Configuration::absolutePath, "retrofe", "RetroFE.exe");
#else
        exePath = Utils::combinePath(Configuration::absolutePath, "RetroFE");
        if (!fs::exists(exePath))
        {
            exePath = Utils::combinePath(Configuration::absolutePath, "retrofe");
        }
#endif

        fs::file_time_type metaDbTime = fs::exists(metaDbPath) ? fs::last_write_time(metaDbPath) : fs::file_time_type::min();
        fs::file_time_type exeTime = fs::exists(exePath) ? fs::last_write_time(exePath) : fs::file_time_type::min();
        fs::file_time_type metadirTime = timeDir(Utils::combinePath(Configuration::absolutePath, "meta"));

        result = (count == 0 || metaDbTime < metadirTime || exeTime < metadirTime) ? true : false;
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
    char* error = nullptr;
    config_.setProperty("status", "Scraping data from \"" + hyperlistFile + "\"");

    std::ifstream file(hyperlistFile.c_str());
    if (!file) {
        LOG_ERROR("Metadata", "Could not open file: " + hyperlistFile);
        return false;
    }

    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    buffer.push_back('\0'); // Null-terminate the buffer

    rapidxml::xml_document<> doc;
    try {
        doc.parse<0>(buffer.data());
        rapidxml::xml_node<>* root = doc.first_node("menu");
        if (!root) {
            LOG_ERROR("Metadata", "Does not appear to be a HyperList file (missing <menu> tag)");
            return false;
        }

        sqlite3* handle = db_.handle;
        sqlite3_exec(handle, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &error);

        sqlite3_stmt* stmt;
        const char* sql = "INSERT OR REPLACE INTO Meta (name, title, year, manufacturer, developer, genre, players, ctrltype, buttons, joyways, cloneOf, collectionName, rating, score) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            LOG_ERROR("Metadata", "SQL Error preparing statement");
            return false;
        }

        for (auto* game = root->first_node("game"); game; game = game->next_sibling("game")) {
            const char* name = game->first_attribute("name") ? game->first_attribute("name")->value() : "";
            if (name[0] == '\0') continue;  
            
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, game->first_node("description") ? game->first_node("description")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, game->first_node("year") ? game->first_node("year")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, game->first_node("manufacturer") ? game->first_node("manufacturer")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, game->first_node("developer") ? game->first_node("developer")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, game->first_node("genre") ? game->first_node("genre")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, game->first_node("players") ? game->first_node("players")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, game->first_node("ctrltype") ? game->first_node("ctrltype")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, game->first_node("buttons") ? game->first_node("buttons")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 10, game->first_node("joyways") ? game->first_node("joyways")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 11, game->first_node("cloneof") ? game->first_node("cloneof")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 12, collectionName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 13, game->first_node("rating") ? game->first_node("rating")->value() : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 14, game->first_node("score") ? game->first_node("score")->value() : "", -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                LOG_ERROR("Metadata", "SQL Error executing statement");
                sqlite3_finalize(stmt);
                return false;
            }
            sqlite3_reset(stmt); // Reset the prepared statement for reuse
        }

        sqlite3_finalize(stmt);
        sqlite3_exec(handle, "COMMIT TRANSACTION;", nullptr, nullptr, &error);
        config_.setProperty("status", "Saving data from \"" + hyperlistFile + "\" to database");
        return true;
    }
    catch (rapidxml::parse_error& e) {
        std::string what = e.what();
        auto line = static_cast<long>(std::count(&buffer.front(), e.where<char>(), char('\n')) + 1);
        std::stringstream ss;
        ss << "Could not parse layout file. [Line: " << line << "] Reason: " << e.what();

        LOG_ERROR("Metadata", ss.str());
    }
    catch (std::exception& e) {
        std::string what = e.what();
        LOG_ERROR("Metadata", "Could not parse hyperlist file. Reason: " + what);
    }
    return false;
}

bool MetadataDatabase::importMamelist(const std::string& filename, const std::string& collectionName)
{
    rapidxml::xml_document<> doc;
    rapidxml::xml_node<> const* rootNode;
    char* error = nullptr;
    sqlite3* handle = db_.handle;

    config_.setProperty("status", "Scraping data from \"" + filename + "\" (this will take a while)");

    LOG_INFO("Mamelist", "Importing mamelist file \"" + filename + "\" (this will take a while)");
    std::ifstream file(filename.c_str());

    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    buffer.push_back('\0');

    doc.parse<0>(&buffer[0]);

    rootNode = doc.first_node("mame");

    if (!rootNode)
    {
        LOG_ERROR("Metadata", "Does not appear to be a MameList file (missing <mame> tag)");
        return false;
    }

    if (sqlite3_exec(handle, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &error) != SQLITE_OK)
    {
        std::string emsg = error;
        LOG_ERROR("Metadata", "SQL Error starting transaction: " + emsg);
        return false;
    };
    std::string gameNodeName = "game";

    // support new mame formats
    if (rootNode->first_node(gameNodeName.c_str()) == nullptr) {
        gameNodeName = "machine";
    }

    sqlite3_stmt* stmt;

    sqlite3_prepare_v2(handle,
        "INSERT OR REPLACE INTO Meta (name, title, year, manufacturer, genre, players, buttons, cloneOf, collectionName) VALUES (?,?,?,?,?,?,?,?,?)",
        -1, &stmt, nullptr);


    for (rapidxml::xml_node<> const* game = rootNode->first_node(gameNodeName.c_str()); game; game = game->next_sibling())
    {
        rapidxml::xml_attribute<> const* nameNode = game->first_attribute("name");
        rapidxml::xml_attribute<> const* cloneOfXml = game->first_attribute("cloneof");

        if (nameNode != nullptr)
        {
            std::string name = nameNode->value();
            rapidxml::xml_node<> const* descriptionNode = game->first_node("description");
            rapidxml::xml_node<> const* yearNode = game->first_node("year");
            rapidxml::xml_node<> const* manufacturerNode = game->first_node("manufacturer");
            rapidxml::xml_node<> const* genreNode = game->first_node("genre");
            rapidxml::xml_node<> const* inputNode = game->first_node("input");

            std::string description = (descriptionNode == nullptr) ? nameNode->value() : descriptionNode->value();
            std::string year = (yearNode == nullptr) ? "" : yearNode->value();
            std::string manufacturer = (manufacturerNode == nullptr) ? "" : manufacturerNode->value();
            std::string genre = (genreNode == nullptr) ? "" : genreNode->value();
            std::string cloneOf = (cloneOfXml == nullptr) ? "" : cloneOfXml->value();
            std::string players;
            std::string buttons;

            if (inputNode != nullptr)
            {
                rapidxml::xml_attribute<> const* playersAttribute = inputNode->first_attribute("players");
                rapidxml::xml_attribute<> const* buttonsAttribute = inputNode->first_attribute("buttons");

                if (playersAttribute)
                {
                    players = playersAttribute->value();
                }

                if (buttonsAttribute)
                {
                    buttons = buttonsAttribute->value();
                }

            }



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
            sqlite3_reset(stmt);
        }
    }

    sqlite3_finalize(stmt);

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


fs::file_time_type MetadataDatabase::timeDir(const std::string& path)
{
    fs::file_time_type lastTime = fs::file_time_type::min();

    for (const auto& entry : fs::recursive_directory_iterator(path))
    {
        if (!fs::is_regular_file(entry) && !fs::is_directory(entry)) {
            continue;
        }

        auto currentFileTime = fs::last_write_time(entry);

        if (currentFileTime > lastTime) {
            lastTime = currentFileTime;
        }
    }

    return lastTime;
}