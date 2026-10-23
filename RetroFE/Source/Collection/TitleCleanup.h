#pragma once

#include "Item.h"
#include <unordered_set>

inline void stripTitleGroups(std::string& title, char opening, char closing) {
    auto first = title.find(opening);
    if (first == std::string::npos) return;
    auto last = title.find(closing, first + 1);
    if (last == std::string::npos) return;
    std::string cleaned;
    cleaned.reserve(title.size());
    std::size_t read = 0;
    do {
        cleaned.append(title, read, first - read);
        read = last + 1;
        first = title.find(opening, read);
        if (first == std::string::npos) break;
        last = title.find(closing, first + 1);
    } while (last != std::string::npos);
    cleaned.append(title, read, std::string::npos);
    title.swap(cleaned);
}

inline void cleanPlaylistTitles(const CollectionInfo::Playlists_T& playlists,
    bool showParenthesis, bool showSquareBrackets) {
    if (showParenthesis && showSquareBrackets) return;
    std::unordered_set<Item*> visited;
    for (const auto& [name, items] : playlists) {
        if (!items) continue;
        for (auto* item : *items) {
            if (!visited.insert(item).second) continue;
            if (!showParenthesis) stripTitleGroups(item->title, '(', ')');
            if (!showSquareBrackets) stripTitleGroups(item->title, '[', ']');
        }
    }
}
