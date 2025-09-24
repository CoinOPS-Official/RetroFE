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
#include <filesystem>

class DB;
class Configuration;
class CollectionInfo;

class MetadataDatabase {
public:
    MetadataDatabase(DB& db, Configuration& c);
    virtual ~MetadataDatabase();

    // Creates schema if needed, always syncs remotes (merge-on-update),
    // then imports when necessary.
    bool initialize();

    // Drops & recreates schema, then initialize() again.
    bool resetDatabase();

    // Import a single HyperList file into the Meta table.
    bool importHyperlist(const std::string& hyperlistFile, const std::string& collectionName);

    // Optional: one-shot fetch+merge for a single file, then import it.
    // Returns true if the local XML changed on disk.
    bool updateAndImportHyperlist(const std::string& remoteRawUrl,
        const std::string& localXmlPath,
        const std::string& collectionName);

    // Copies DB metadata into a collection’s items.
    void injectMetadata(CollectionInfo* collection);

private:
    bool needsRefresh();
    std::filesystem::file_time_type timeDir(const std::string& path);

    // Scan meta/hyperlist for *.xml, read optional .remote sidecar,
    // fetch+merge if remote newer. Returns true if any local file changed.
    bool syncAllHyperlistRemotes_();

    // Import all local *.xml (no network).
    bool importAllHyperlists_();

    Configuration& config_;
    DB& db_;
};