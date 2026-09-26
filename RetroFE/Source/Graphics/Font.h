// Font.h

#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <string>
#include <cstdint>
#include <map>
#include <set>

class TextEngineAtlas;

class FontManager {
public:
    // Each level owns a font face at one prepared raster size.
    struct MipLevel {
        int fontSize = 0;
        int outlinePx = 0;
        int height = 0, ascent = 0, descent = 0;
        TTF_Font* font = nullptr;
        TTF_Font* outlineFont = nullptr;
        TTF_Font* fallbackFont = nullptr;
        TextEngineAtlas* textEngineAtlas = nullptr;
        ~MipLevel();
    };

    // MODIFIED: The constructor now takes a maximum font size and optional fallback font path.
    FontManager(std::string fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor, std::string fallbackFontPath = "");
    ~FontManager();

    // Lifetime methods
    bool initialize();
    void deInitialize();
    uint64_t getResourceGeneration() const { return resourceGeneration_; }

    // Styling knobs
    void setOutline(int px, SDL_Color color) { outlinePx_ = (px < 0 ? 0 : px); outlineColor_ = color; }
    void setColor(SDL_Color c);

    // --- NEW AND MODIFIED QUERIES ---

    // NEW: The primary method to get font data for rendering.
    // Read-only ceiling selection; oversized requests use the largest prepared atlas.
    const MipLevel* getMipLevelForSize(float targetSize) const;
    const MipLevel* getMipLevelForHeight(float targetHeight) const;
    // Preparation only: called while parsing layouts, never by draw/select.
    bool prepareSize(float size);
    bool prepareHeight(float height);

    // Stable loadFontSize reference metrics, retained for height-based layout.
    int       getMaxHeight()   const { return max_height_; }
    int       getMaxAscent()   const { return max_ascent_; }
    int       getMaxFontSize() const { return maxFontSize_; }
    SDL_Color getColor()       const { return color_; }

    int getWidth(const std::string& text);
    int getWidth(const std::string& text, const MipLevel& mip) const;
    float getWidthForHeight(const std::string& text, float height) const;
    int getOutlinePx() const;

    const std::string& getFontPath() const { return fontPath_; }
    const std::string& getFallbackFontPath() const { return fallbackFontPath_; }
    bool               getGradient() const { return gradient_; }
    int                getMonitor()  const { return monitor_; }

    TextEngineAtlas* getTextEngineAtlas(const MipLevel* mip);
    bool prewarmTextEngine(const MipLevel* mip);

private:
    // Config
    std::string fontPath_;
    std::string fallbackFontPath_;
    int maxFontSize_ = 0; // MODIFIED: Renamed from fontSize_
    SDL_Color color_{ 255,255,255,255 };
    int monitor_ = 0;
    bool gradient_;
    int outlinePx_;
    SDL_Color outlineColor_{ 0,0,0,255 };

    // --- MODIFIED RUNTIME MEMBERS ---

    // Stable reference metrics for height-based layout.
    int max_height_ = 0, max_ascent_ = 0;

    // std::map keeps the sizes sorted, which makes finding the best fit easy.
    std::map<int, MipLevel*> mipLevels_;
    std::set<int> preparedSizes_;
    uint64_t resourceGeneration_ = 0;

    // Internal helpers
    void clearMips();
    bool buildMip(int currentSize);
};
