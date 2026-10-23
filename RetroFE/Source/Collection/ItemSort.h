#pragma once

#include "Item.h"
#include <algorithm>
#include <string>
#include <vector>

// Keys live only for this sort: later metadata/title/play-count edits need no
// cache invalidation. Keep the existing lexical metadata comparisons.
inline void sortItemsByKeys(std::vector<Item*>& items, const std::string& sortType,
    bool menuSort, bool history = false) {
    if (items.size() < 2) return;
    struct Key {
        Item* item;
        std::string collection;
        std::string value;
        std::string title;
    };
    std::vector<Key> keys;
    keys.reserve(items.size());
    for (auto* item : items) {
        keys.push_back({item, item->collectionInfo->lowercaseName(),
            sortType.empty() ? std::string{} : item->getMetaAttribute(sortType),
            menuSort || history ? item->lowercaseFullTitle() : std::string{}});
    }
    const bool descending = Item::isSortDesc(sortType);
    std::sort(keys.begin(), keys.end(), [&](const Key& lhs, const Key& rhs) {
        const auto* a = lhs.item;
        const auto* b = rhs.item;
        if (a->leaf != b->leaf) return a->leaf;
        if (a->collectionInfo->subsSplit && a->collectionInfo != b->collectionInfo)
            return lhs.collection < rhs.collection;
        if (!a->leaf && !b->leaf && (!history || !a->collectionInfo->menusort)) return false;
        if (!sortType.empty() && lhs.value != rhs.value)
            return descending ? lhs.value > rhs.value : lhs.value < rhs.value;
        if (!menuSort && !history) return false;
        return lhs.title < rhs.title;
    });
    for (std::size_t i = 0; i < keys.size(); ++i) items[i] = keys[i].item;
}
