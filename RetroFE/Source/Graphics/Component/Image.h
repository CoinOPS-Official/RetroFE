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
#include <shared_mutex>
#include <vector>

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
     */
    Image(const std::string& file, const std::string& altFile, Page& p, int monitor, bool additive, bool useTextureCaching = false);

    /**
     * @brief Destructor. Ensures that graphics memory is freed.
     */
    ~Image() override;

    /**
     * @brief Allocates graphics memory for the image, utilizing the texture cache.
     */
    void allocateGraphicsMemory() override;

    /**
     * @brief Frees graphics memory associated with the image.
     *        Does not destroy cached textures or animations.
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
    /**
     * @brief Structure to hold cached image data.
     *        Can represent either a static texture or an animated image.
     */
    struct CachedImage {
        SDL_Texture* texture = nullptr;                    // For static images
        std::vector<SDL_Texture*> frameTextures;           // Frame textures for animations
        int frameDelay = 0;                                // Single delay time for all frames
    };

    // Texture Cache: Maps file paths to their cached resources
    static std::unordered_map<std::string, CachedImage> textureCache_;
    static std::shared_mutex textureCacheMutex_;                   // Mutex for thread-safe access to the cache

    /**
     * @brief Loads the contents of a file into a buffer.
     *
     * @param filePath The path to the file.
     * @param buffer   The buffer to store the file contents.
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
    static bool isAnimatedGIF(const std::vector<uint8_t>& buffer);

    /**
     * @brief Checks if a buffer contains WebP data based on magic numbers.
     *
     * @param buffer The buffer containing file data.
     * @return true  If the buffer represents a WebP image.
     * @return false Otherwise.
     */
    static bool isAnimatedWebP(const std::vector<uint8_t>& buffer);

    // Member variables
    std::string file_;                                      // Primary file path
    std::string altFile_;                                   // Alternative file path
    SDL_Texture* texture_ = nullptr;                        // Static texture
    const std::vector<SDL_Texture*>* frameTextures_ = nullptr; // Pointer to cached frame textures
    size_t currentFrame_ = 0;                                  // Current frame index for animations
    Uint32 lastFrameTime_ = 0;                              // Timestamp of the last frame update
	int frameDelay_ = 0;                                    // Delay time for the current frame 
    bool textureIsUncached_ = false;
	bool useTextureCaching_ = false;
    std::vector<uint8_t> buffer_; // Persistent buffer
};
