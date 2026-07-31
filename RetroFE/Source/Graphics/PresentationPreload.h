#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class FontManager;
class Item;

struct PresentationPreloadContext {
    const std::vector<Item*>* items = nullptr;
    std::size_t selectedIndex = 0;

    Item* itemAtOffset(int offset) const {
        if (!items || items->empty()) {
            return nullptr;
        }

        const std::size_t size = items->size();
        const std::int64_t signedOffset = offset;
        const std::size_t distance =
            static_cast<std::size_t>(
                signedOffset >= 0
                    ? signedOffset
                    : -signedOffset
            ) % size;
        const std::size_t index = signedOffset >= 0
            ? (selectedIndex + distance) % size
            : (selectedIndex + size - distance) % size;
        return (*items)[index];
    }
};

struct PresentationImageRequest {
    std::string path;
    int monitor = 0;
    bool additive = false;
    unsigned int layer = 0;
};

struct PresentationTextRequest {
    FontManager* font = nullptr;
    std::string text;
    int fontSize = 0;
    unsigned int layer = 0;
};

struct PresentationPreloadContribution {
    std::size_t listImages = 0;
    std::size_t textFallbacks = 0;
    std::size_t videosSkipped = 0;
};

class PresentationPreloadCollector {
public:
    void addImage(
        std::string path,
        int monitor,
        bool additive,
        unsigned int layer)
    {
        if (path.empty()) {
            return;
        }

        images.push_back({
            std::move(path),
            monitor,
            additive,
            layer
        });
    }

    void addText(
        FontManager* font,
        std::string text,
        int fontSize,
        unsigned int layer)
    {
        if (!font || text.empty()) {
            return;
        }

        texts.push_back({
            font,
            std::move(text),
            fontSize,
            layer
        });
    }

    std::vector<PresentationImageRequest> images;
    std::vector<PresentationTextRequest> texts;
};
