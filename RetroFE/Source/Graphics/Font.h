// Font.h

#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <string>
#include <cstdint>
#include <unordered_map>
#include <map>
#include <set>
#include <vector>  // NEW: needed for std::vector<TmpGlyph>

class FontManager {
public:
    struct GlyphInfo {
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        int advance = 0;
        SDL_Rect rect{ 0,0,0,0 };
        int fillX = 0, fillY = 0, fillW = 0, fillH = 0;
        int topPad = 0; // rows of mostly-transparent pixels at top of packed glyph
    };

    // A structure to hold all data for a single mipmap level.
    // Each MipLevel corresponds to a specific font size.
    struct MipLevel {
        int fontSize = 0;
        int outlinePx = 0;
        int height = 0, ascent = 0, descent = 0;
        SDL_Texture* fillTexture = nullptr;      // prebuilt atlas (static glyphs)
        SDL_Texture* outlineTexture = nullptr;   // prebuilt outlines (optional)

        // Per-mip persistent font handle (prevents reopen/close per glyph)
        TTF_Font* font = nullptr;

        // Dynamic (on-demand) atlas
        SDL_Texture* dynamicFillTexture = nullptr;     // STREAMING
        SDL_Texture* dynamicOutlineTexture = nullptr;  // STREAMING (optional)
        std::unordered_map<Uint32, GlyphInfo> dynamicGlyphs;

        // Static glyph map (prebuilt in initialize)
        std::unordered_map<Uint32, GlyphInfo> glyphs;

        // Simple shelf packing
        int dynamicAtlasSize = 0;
        int dynamicNextX = 0;
        int dynamicNextY = 0;
        int dynamicRowHeight = 0;

        int atlasW = 0, atlasH = 0;

        ~MipLevel();
    };

    // MODIFIED: The constructor now takes a maximum font size.
    FontManager(std::string fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor);
    ~FontManager();

    // Lifetime methods
    bool initialize();
    void deInitialize();
    uint64_t getResourceGeneration() const { return resourceGeneration_; }

    // Styling knobs (mostly unchanged)
    void setOutline(int px, SDL_Color color) { outlinePx_ = (px < 0 ? 0 : px); outlineColor_ = color; }
    void setColor(SDL_Color c); // Will now apply color to all mip levels.

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

    // Legacy overloads use loadFontSize; selected-mip overloads match drawing.
    int getKerning(Uint32 prevChar, Uint32 curChar) const;  // ? was Uint16
    int getKerning(const MipLevel& mip, Uint32 prevChar, Uint32 curChar) const;
    int getWidth(const std::string& text);
    int getWidth(const std::string& text, const MipLevel& mip) const;
    float getWidthForHeight(const std::string& text, float height) const;
    int getOutlinePx() const;  // FIXED: removed trailing backslash

    const std::string& getFontPath() const { return fontPath_; }
    bool               getGradient() const { return gradient_; }
    int                getMonitor()  const { return monitor_; }

    // NEW: On-demand glyph loading
    bool loadGlyphOnDemand(Uint32 ch, MipLevel* mip);

private:
    // Helper structures for building glyphs during initialization
    struct GlyphInfoBuild {
        GlyphInfo glyph;
        SDL_Surface* surface = nullptr;
    };

    struct TmpGlyph {
        SDL_Surface* fill = nullptr;
        SDL_Surface* outline = nullptr;
        int dx = 0, dy = 0;
        GlyphInfoBuild* info = nullptr;
    };

    // Config
    std::string fontPath_;
    int maxFontSize_ = 0; // MODIFIED: Renamed from fontSize_
    SDL_Color color_{ 255,255,255,255 };
    int monitor_ = 0;
    bool gradient_;
    int outlinePx_;
    SDL_Color outlineColor_{ 0,0,0,255 };

    // --- MODIFIED RUNTIME MEMBERS ---

    // The loadFontSize handle is the stable reference for legacy height-based layout.
    // Exact requested raster sizes may also be prepared above this size.
    TTF_Font* max_font_ = nullptr;
    int max_height_ = 0, max_descent_ = 0, max_ascent_ = 0;

    // std::map keeps the sizes sorted, which makes finding the best fit easy.
    std::map<int, MipLevel*> mipLevels_;
    std::set<int> preparedSizes_;
    uint64_t resourceGeneration_ = 0;

    // Internal helpers
    void clearMips();
    bool buildMip(int currentSize);
    static SDL_Surface* applyVerticalGrayGradient(SDL_Surface* s, Uint8 topGray = 255, Uint8 bottomGray = 64);
    static void fillHolesInOutline(SDL_Surface* s, int alphaThresh, int minHoleArea, int minHoleW, int minHoleH);

    // NEW: Helper to preload a range of glyphs into the atlas
    void preloadGlyphRange(TTF_Font* font, int outlinePx,
        Uint32 start, Uint32 end,
        int& x, int& y,
        int atlasWidth, int& atlasHeight,
        int GLYPH_SPACING,
        std::vector<TmpGlyph>& tmp,
        std::unordered_map<unsigned int, GlyphInfoBuild*>& temp_build);
};
