#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace PlaylistRules {
inline bool collectionIsSkipped(std::string_view csv, std::string_view collection) {
    while (!csv.empty()) {
        const auto comma = csv.find(',');
        if (csv.substr(0, comma) == collection) return true;
        if (comma == std::string_view::npos) break;
        csv.remove_prefix(comma + 1);
    }
    return false;
}

// The caller has already advanced once. Keep that candidate if it is usable,
// otherwise search forward, wrapping at most once. Missing cycle entries are
// not selectable, and an exhausted cycle leaves the current playlist alone.
template <typename IsSelectable>
auto findAttractPlaylist(const std::vector<std::string>& cycle,
    const std::string& current, IsSelectable isSelectable) {
    auto candidate = std::find(cycle.begin(), cycle.end(), current);
    if (candidate == cycle.end()) candidate = cycle.begin();
    for (std::size_t attempts = 0; attempts < cycle.size(); ++attempts) {
        if (isSelectable(*candidate)) return candidate;
        if (++candidate == cycle.end()) candidate = cycle.begin();
    }
    return cycle.end();
}
}
