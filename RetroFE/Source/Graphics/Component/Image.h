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
#ifdef __APPLE__
#include <SDL2_image/SDL_image.h>
#else
#include <SDL_image.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <vector>
#include <memory>    // For std::align and alignment requirements

// Platform-specific aligned allocation
#if defined(_MSC_VER)
#include <malloc.h>
#define ALIGNED_MALLOC(size, alignment) _aligned_malloc(size, alignment)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <cstdlib>
#define ALIGNED_MALLOC(size, alignment) aligned_alloc(alignment, size)
#define ALIGNED_FREE(ptr) free(ptr)
#endif

class Image : public Component {
public:
    /**
    * @brief Constructs an Image instance.
    *
    * @param file      The primary file path of the image.
    * @param altFile   The alternative file path if the primary fails.
    * @param p         Reference to the current Page.
    * @param monitor   Monitor index where the image will be displayed.
    * @param additive  Flag indicating whether additive blending should be used.
    * @param useTextureCaching Flag indicating whether texture caching should be used.
    */
    Image(const std::string& file, const std::string& altFile, Page& p, int monitor, bool additive, bool useTextureCaching = false);

    /**
    * @brief Destructor. Ensures that graphics memory is freed.
    */
    ~Image() override;

    /**
    * @brief Allocates graphics memory for the image, utilizing the texture cache if enabled.
    */
    void allocateGraphicsMemory() override;

    /**
    * @brief Frees graphics memory associated with the image.
    *        Destroys uncached textures if caching is disabled.
    */
    void freeGraphicsMemory() override;

    /**
    * @brief Renders the image onto the screen.
    *        Handles both static and animated images.
    */
    void draw() override;

    /**
    * @brief Retrieves the primary file path of the image.
    *
    * @return std::string_view The primary file path.
    */
    std::string_view filePath() override;

    /**
    * @brief Cleans up the entire texture cache by destroying all cached resources.
    *        Should be called once during application shutdown.
    */
    static void cleanupTextureCache();

private:
    static constexpr size_t ALIGNMENT = 32;  // AVX-256 alignment

    template<typename T>
    class AlignedAllocator {
    public:
        static constexpr size_t alignment = ALIGNMENT;

        using value_type = T;
        using pointer = T*;
        using const_pointer = const T*;
        using reference = T&;
        using const_reference = const T&;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_copy_assignment = std::false_type;
        using propagate_on_container_swap = std::false_type;
        using is_always_equal = std::true_type;

        template<typename U>
        struct rebind { using other = AlignedAllocator<U>; };

        AlignedAllocator() noexcept = default;
        template<typename U>
        explicit AlignedAllocator(const AlignedAllocator<U>&) noexcept {}

        [[nodiscard]] pointer allocate(size_type n) {
            if (n == 0) return nullptr;
            constexpr size_type align_mask = alignment - 1;
            const size_type alignedSize = ((n * sizeof(T)) + align_mask) & ~align_mask;

            void* ptr = ALIGNED_MALLOC(alignedSize, alignment);
            if (!ptr) throw std::bad_alloc();
            return static_cast<T*>(ptr);
        }

        void deallocate(pointer p, size_type) noexcept {
            ALIGNED_FREE(p);
        }

        template<typename U>
        bool operator==(const AlignedAllocator<U>&) const noexcept { return true; }

        template<typename U>
        bool operator!=(const AlignedAllocator<U>&) const noexcept { return false; }
    };



    class PathCache {
    public:
        struct CacheKey {
            std::string_view directory;  // Reference to pooled directory path
            std::string_view filename;   // Reference to pooled filename
            int monitor;

            // Required for use as unordered_map key
            bool operator==(const CacheKey& other) const {
                return monitor == other.monitor &&
                    directory == other.directory &&
                    filename == other.filename;
            }
        };

        struct CacheKeyHash {
            size_t operator()(const CacheKey& key) const {
                // Efficient hash combination
                size_t h1 = std::hash<std::string_view>{}(key.directory);
                size_t h2 = std::hash<std::string_view>{}(key.filename);
                return h1 ^ (h2 << 1) ^ (std::hash<int>{}(key.monitor) << 2);
            }
        };

    private:
        std::unordered_set<std::string> directories_;  // Pool of unique directory paths
        std::unordered_set<std::string> filenames_;    // Pool of unique filenames
        std::mutex cacheMutex_;

    public:
        /**
        * @brief Creates or retrieves a cached key for the given path.
        *
        * @param filePath Full path to the image file
        * @param monitor Monitor index
        * @return CacheKey Structure containing references to cached path components
        */
        CacheKey getKey(const std::string& filePath, int monitor);
    };

    struct CachedImage {
        SDL_Texture* texture = nullptr;
        std::vector<SDL_Texture*> frameTextures;
        int frameDelay = 0;
    };

    // Static members for texture caching
    static PathCache pathCache_;
    static std::unordered_map<PathCache::CacheKey, CachedImage, PathCache::CacheKeyHash> textureCache_;
    static std::shared_mutex textureCacheMutex_;

    /**
    * @brief Loads the contents of a file into a buffer.
    *
    * @param filePath The path to the file.
    * @return true    If the file was loaded successfully.
    * @return false   If the file could not be loaded.
    */
    bool loadFileToBuffer(const std::string& filePath);

    /**
    * @brief Checks if a buffer contains GIF data based on magic numbers.
    *
    * @param buffer The buffer containing file data.
    * @return true  If the buffer represents a GIF image.
    * @return false Otherwise.
    */
    static bool isAnimatedGIF(const std::vector<uint8_t, AlignedAllocator<uint8_t>>& buffer);

    /**
    * @brief Checks if a buffer contains WebP data based on magic numbers.
    *
    * @param buffer The buffer containing file data.
    * @return true  If the buffer represents a WebP image.
    * @return false Otherwise.
    */
    static bool isAnimatedWebP(const std::vector<uint8_t, AlignedAllocator<uint8_t>>& buffer);

    // Member variables
    std::string file_;                                      // Primary file path
    std::string altFile_;                                   // Alternative file path
    SDL_Texture* texture_ = nullptr;                        // Static texture
    std::vector<SDL_Texture*> frameTextures_;               // Vector of frame textures for animated images
    size_t currentFrame_ = 0;                               // Current frame index for animations
    Uint32 lastFrameTime_ = 0;                              // Timestamp of the last frame update
    int frameDelay_ = 0;                                    // Delay time for the current frame 
    bool textureIsUncached_ = false;                        // Flag indicating if texture is uncached
    bool useTextureCaching_ = false;                        // Flag indicating if texture caching should be used
    std::vector<uint8_t, AlignedAllocator<uint8_t>> buffer_; // Aligned buffer for image data
};

#undef ALIGNED_MALLOC
#undef ALIGNED_FREE