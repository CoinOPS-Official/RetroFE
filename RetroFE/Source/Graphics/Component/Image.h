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
#include "Component.h"
#include <SDL2/SDL.h>
#if __has_include(<SDL2/SDL_image.h>)
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
#include <SDL2_image/SDL_image.h>
#else
#error "Cannot find SDL_image header"
#endif
#ifdef __APPLE__
#include <webp/decode.h>
#include <webp/demux.h>
#elif defined(_WIN32) 
#include <SDL_image.h>
#include <decode.h>
#include <demux.h>
#else  // Assume Linux
#include <webp/decode.h>
#include <webp/demux.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <vector>

class Image : public Component {
public:
    //-------------------------------------------------------------------------
    // Public Interface
    //-------------------------------------------------------------------------
    Image(const std::string& file, const std::string& altFile, Page& p,
        int monitor, bool additive, bool useTextureCaching = false);
    ~Image() override;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    // Core Component Operations
    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    void draw() override;
    std::string_view filePath() override;

    // Static Cache Management
    static void cleanupTextureCache();
    void retarget(const std::string& newFile, const std::string& newAltFile);


private:
    //-------------------------------------------------------------------------
    // Cache Infrastructure
    //-------------------------------------------------------------------------
    class PathCache {
    public:
        struct CacheKey {
            std::string_view directory;  // Reference to pooled directory path
            std::string_view filename;   // Reference to pooled filename
            int monitor;

            bool operator==(const CacheKey& other) const {
                return monitor == other.monitor &&
                    directory == other.directory &&
                    filename == other.filename;
            }
        };

        struct CacheKeyHash {
            size_t operator()(const CacheKey& key) const {
                size_t h1 = std::hash<std::string_view>{}(key.directory);
                size_t h2 = std::hash<std::string_view>{}(key.filename);
                return h1 ^ (h2 << 1) ^ (std::hash<int>{}(key.monitor) << 2);
            }
        };

        CacheKey getKey(const std::string& filePath, int monitor);

    private:
        std::unordered_set<std::string> directories_;  // Pool of unique directory paths
        std::unordered_set<std::string> filenames_;    // Pool of unique filenames
        std::mutex cacheMutex_;
    };

    struct CachedImage {
        SDL_Texture* texture = nullptr;
        SDL_Texture* animatedTexture = nullptr;     // For animated images.
        int frameDelay = 0;
        std::vector<SDL_Surface*> animatedSurfaces;
    };

    //-------------------------------------------------------------------------
    // Loading Context
    //-------------------------------------------------------------------------
    struct LoadContext {
        const std::string& filePath;
        PathCache::CacheKey cacheKey;
        CachedImage& newCachedImage;
        ViewInfo& baseViewInfo;
        std::vector<SDL_Surface*>& animatedSurfaces;
        int& frameDelay;
        Uint32& lastFrameTime;
        bool useCache;
    };

    //-------------------------------------------------------------------------
    // Private Loading Functions
    //-------------------------------------------------------------------------

    bool loadFromCache(const LoadContext& ctx);
    bool validateSurfaces(const std::vector<SDL_Surface*>& surfaces) const;
    bool loadStaticImage(const std::vector<uint8_t>& buffer, LoadContext& ctx);
    bool loadAnimatedWebP(const std::vector<uint8_t>& buffer, LoadContext& ctx);
    bool loadNextWebPFrame(LoadContext& ctx);
    bool loadAnimatedGIF(const std::vector<uint8_t>& buffer, LoadContext& ctx);
    static bool loadFileToBuffer(const std::string& filePath, std::vector<uint8_t>& outBuffer);

    // Format Detection
    static bool isAnimatedGIF(const std::vector<uint8_t>& buffer);
    static bool isAnimatedWebP(const std::vector<uint8_t>& buffer);

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    // Resource paths
    std::string file_;
    std::string altFile_;

    // Texture management
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* animatedTexture_ = nullptr;
    std::vector<SDL_Surface*> animatedSurfaces_;


    // Animation state
    size_t currentFrame_ = 0;
    Uint32 lastFrameTime_ = 0;
    int frameDelay_ = 0;

    // Caching control
    bool textureIsUncached_ = false;
    bool useTextureCaching_ = false;
    bool isUsingCachedSurfaces_ = false;

    // Static cache storage
    static PathCache pathCache_;
    static std::unordered_map<PathCache::CacheKey, CachedImage, PathCache::CacheKeyHash> textureCache_;
    static std::shared_mutex textureCacheMutex_;
};
