// PlaylistPath.h
#pragma once
#include <string>
#include <filesystem>
#include "../Utility/Utils.h"
#include "../Database/Configuration.h"

namespace PlaylistPath {

    // Normalize to a *RetroFE-root-relative* path and lexically_normal it.
    inline std::filesystem::path normalizeRel(const std::string& rel) {
        namespace fs = std::filesystem;
        fs::path p(rel);
        if (Utils::isAbsolutePath(rel)) {
            // make it relative if under RetroFE root
            if (Utils::isSubPath(rel)) {
                return fs::relative(p, Configuration::absolutePath).lexically_normal();
            }
            return p.lexically_normal(); // absolute outside root (shouldn't happen w/ policy)
        }
        return fs::path(rel).lexically_normal();
    }

    // Is `collections/<collection>/playlists/<file>.txt` ?
    inline bool isPlaylistPath(const std::string& rel) {
        namespace fs = std::filesystem;
        fs::path p = normalizeRel(rel);

        if (p.extension() != ".txt") return false;

        fs::path playlistsDir = p.parent_path();               // .../playlists
        fs::path collectionDir = playlistsDir.parent_path();    // .../<collection>
        fs::path collectionsDir = collectionDir.parent_path();   // .../collections

        return !p.empty()
            && playlistsDir.filename() == "playlists"
            && collectionsDir.filename() == "collections"
            && !collectionDir.filename().empty();
    }

    // Extract <collection> and <playlist> (stem of filename)
    inline bool tryParse(const std::string& rel, std::string& collection, std::string& playlist) {
        if (!isPlaylistPath(rel)) return false;
        namespace fs = std::filesystem;
        fs::path p = normalizeRel(rel);

        fs::path playlistsDir = p.parent_path();
        fs::path collectionDir = playlistsDir.parent_path();

        collection = collectionDir.filename().string();
        playlist = p.stem().string();
        return !collection.empty() && !playlist.empty();
    }

} // namespace PlaylistPath
