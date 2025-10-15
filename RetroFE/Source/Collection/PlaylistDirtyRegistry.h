// PlaylistDirtyRegistry.h
#pragma once
#include <string>
#include <map>
#include <set>
#include <vector>
#include <mutex>

class PlaylistDirtyRegistry {
public:
    // Public API
    static void addPath(const std::string& localPath);
    static std::vector<std::string> drainForCollection(const std::string& collection);
    static void clear();
    static bool isDirty(const std::string& collection, const std::string& playlist);
    static void clearOne(const std::string& collection, const std::string& playlist);

private:
    // Shared state for all static methods
    static std::mutex m_;
    static std::map<std::string, std::set<std::string>> dirty_;
};
