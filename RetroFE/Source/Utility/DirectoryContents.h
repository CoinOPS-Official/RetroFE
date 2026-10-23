#pragma once

#include "Utils.h"
#include <algorithm>
#include <filesystem>
#include <unordered_map>

// A short-lived snapshot for configuration import. File presence comes from
// one directory enumeration; only requested directory entries need a status.
class DirectoryContents {
public:
    explicit DirectoryContents(const std::filesystem::path& directory) : directory_(directory) {
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
            !error && it != end; it.increment(error))
            entries_.try_emplace(key(it->path().filename().string()), *it);
        complete_ = !error;
    }

    bool contains(const std::string& name) const {
        const auto found = entries_.find(key(name));
        if (found != entries_.end()) return found->second.exists();
        return needsFallback(name) && std::filesystem::exists(directory_ / name);
    }

    bool isDirectory(const std::string& name) const {
        const auto found = entries_.find(key(name));
        if (found != entries_.end()) return found->second.is_directory();
        return needsFallback(name) && std::filesystem::is_directory(directory_ / name);
    }

private:
    static std::string key(const std::string& name) {
#ifdef _WIN32
        return Utils::toLower(name);
#else
        return name;
#endif
    }
    bool needsFallback(const std::string& name) const {
#ifdef __APPLE__
        // A Mac volume can be case-sensitive or case-insensitive.
        return true;
#elif defined(_WIN32)
        // Preserve native matching for Unicode names not handled by byte folding.
        return !complete_ || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c >= 128; });
#else
        return !complete_;
#endif
    }
    std::filesystem::path directory_;
    std::unordered_map<std::string, std::filesystem::directory_entry> entries_;
    bool complete_ = false;
};
