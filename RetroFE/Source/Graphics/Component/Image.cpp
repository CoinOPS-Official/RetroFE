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
#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"           // Ensure this header declares SDL::getRenderer and SDL::getMutex
#include "../../Utility/Log.h"


#if (__APPLE__)
#include <SDL2_image/SDL_image.h>
#else
#include <SDL2/SDL_image.h>
#include <decode.h>
#include <demux.h>

#endif

#include <string_view>
#include <fstream>
#include <vector>
#include <cstring>

 // Definition of static members
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::shared_mutex Image::textureCacheMutex_;

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
    // Use string_view for initial splitting to avoid allocations
    std::string_view pathView(filePath);
    std::string_view directoryView, filenameView;

    // Find last occurrence of either forward slash or backslash
    // Linux uses '/', Windows uses '\', but many Windows APIs accept '/' too
    size_t lastForwardSlash = pathView.find_last_of('/');
    size_t lastBackSlash = pathView.find_last_of('\\');

    // Determine the actual last separator position
    size_t lastSlash;
    if (lastForwardSlash == std::string::npos) {
        lastSlash = lastBackSlash;
    } else if (lastBackSlash == std::string::npos) {
        lastSlash = lastForwardSlash;
    } else {
        lastSlash = std::max(lastForwardSlash, lastBackSlash);
    }

    if (lastSlash != std::string::npos) {
        directoryView = pathView.substr(0, lastSlash);
        filenameView = pathView.substr(lastSlash + 1);

        // Handle empty directory case
        if (directoryView.empty()) {
            directoryView = ".";
        }
    } else {
        directoryView = ".";
        filenameView = pathView;
    }

    std::lock_guard<std::mutex> lock(cacheMutex_);
    // Use emplace for efficient in-place construction
    auto dirIt = directories_.emplace(directoryView).first;
    auto fileIt = filenames_.emplace(filenameView).first;

    return { *dirIt, *fileIt, monitor };
}

bool Image::loadFileToBuffer(const std::string& filePath, std::vector<uint8_t>& outBuffer) 
{
    LOG_INFO("Image", "Attempting to load file into buffer: " + filePath);

    // Open the file in binary mode and move the cursor to the end to determine the size
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR("Image", "Failed to open file: " + filePath);
        return false;
    }

    // Get the file size
    std::streamsize size = file.tellg();
    LOG_INFO("Image", "File size: " + std::to_string(size) + " bytes");
    if (size <= 0) {
        LOG_ERROR("Image", "File is empty or invalid: " + filePath);
        return false;
    }

    // Seek back to the beginning of the file
    file.seekg(0, std::ios::beg);

    // Resize the outBuffer to fit the file data
    outBuffer.resize(static_cast<size_t>(size));

    // Read the file data directly into the outBuffer
    if (!file.read(reinterpret_cast<char*>(outBuffer.data()), size)) {
        LOG_ERROR("Image", "Failed to read file: " + filePath);
        outBuffer.clear(); // Ensure the buffer is empty on failure
        return false;
    }

    LOG_INFO("Image", "Successfully loaded file into buffer: " + filePath);
    return true;
}

bool Image::isAnimatedGIF(const std::vector<uint8_t>& buffer) {
	// Early exit for small files
	if (buffer.size() < 10) return false;

	// Look for the GIF89a or GIF87a signature to ensure it's a valid GIF
	if (!(std::memcmp(buffer.data(), "GIF87a", 6) == 0 || std::memcmp(buffer.data(), "GIF89a", 6) == 0)) {
		return false;
	}

	// Search through the file to see if there are more than one frame separator (0x21, 0xF9)
	size_t frameCount = 0;
	for (size_t i = 0; i < buffer.size() - 1; ++i) {
		if (buffer[i] == 0x21 && buffer[i + 1] == 0xF9) {
			frameCount++;
			if (frameCount > 1) {
				return true; // Animated if more than one frame is found
			}
		}
	}
	return false;
}


bool Image::isAnimatedWebP(const std::vector<uint8_t>& buffer) {
	// Early exit for small files
	if (buffer.size() < 12) return false;

	// Ensure it is a valid WebP file
	if (!(std::memcmp(buffer.data(), "RIFF", 4) == 0 && std::memcmp(buffer.data() + 8, "WEBP", 4) == 0)) {
		return false;
	}

	// Set up WebP data structure
	WebPData webpData = { buffer.data(), buffer.size() };

	// Create the WebP demuxer to inspect the file's structure
	WebPDemuxer* demux = WebPDemux(&webpData);
	if (!demux) {
		LOG_ERROR("Image", "Failed to create WebPDemuxer.");
		return false;
	}

	// Check the number of frames in the WebP animation
	int frameCount = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
	WebPDemuxDelete(demux);

	// If there is more than one frame, the WebP is animated
	return frameCount > 1;
}


Image::Image(const std::string& file, const std::string& altFile, Page& p, int monitor, bool additive, bool useTextureCaching)
	: Component(p), file_(file), altFile_(altFile), useTextureCaching_(useTextureCaching)
{
	baseViewInfo.Monitor = monitor;
	baseViewInfo.Additive = additive;
	baseViewInfo.Layout = page.getCurrentLayout();
}

Image::~Image() {
	Image::freeGraphicsMemory();
}

void Image::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    // Handle static texture cleanup
    if (!useTextureCaching_ && texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    // Handle animated texture cleanup
    if (!useTextureCaching_) {
        for (SDL_Texture* texture : frameTextures_) {
            if (texture) {
                SDL_DestroyTexture(texture);
            }
        }
        frameTextures_.clear();
    }
}

void Image::allocateGraphicsMemory() {
    // Check if graphics memory is already allocated
    if (texture_ || !frameTextures_.empty()) {
        // Graphics memory already allocated
        return;
    }

    // Define a lambda to attempt loading a file with optional caching
    auto tryLoad = [&](const std::string& filePath) -> bool {
        // Generate the cache key using PathCache
        auto cacheKey = pathCache_.getKey(filePath, baseViewInfo.Monitor);

        if (useTextureCaching_) {
            // Attempt to retrieve CachedImage from cache
            {
                std::shared_lock<std::shared_mutex> lock(textureCacheMutex_);
                auto it = textureCache_.find(cacheKey);
                if (it != textureCache_.end()) {
                    CachedImage& cachedImage = it->second;
                    if (cachedImage.texture) {
                        texture_ = cachedImage.texture;
                        int width, height;
                        if (SDL_QueryTexture(texture_, nullptr, nullptr, &width, &height) == 0) {
                            baseViewInfo.ImageWidth = static_cast<float>(width);
                            baseViewInfo.ImageHeight = static_cast<float>(height);
                            LOG_INFO("Image", "Loaded static texture from cache: " + filePath);
                            return true;
                        }
                        LOG_ERROR("Image", "Failed to query cached texture: " + std::string(SDL_GetError()));
                        return false;
                    }
                    else if (!cachedImage.frameTextures.empty()) {
                        frameTextures_ = cachedImage.frameTextures;
                        frameDelay_ = cachedImage.frameDelay;
                        SDL_Texture* firstFrame = cachedImage.frameTextures[0];
                        int width, height;
                        if (SDL_QueryTexture(firstFrame, nullptr, nullptr, &width, &height) == 0) {
                            baseViewInfo.ImageWidth = static_cast<float>(width);
                            baseViewInfo.ImageHeight = static_cast<float>(height);
                            lastFrameTime_ = SDL_GetTicks();
                            LOG_INFO("Image", "Loaded animated texture from cache: " + filePath);
                            return true;
                        }
                        LOG_ERROR("Image", "Failed to query first frame of cached animated texture: " + std::string(SDL_GetError()));
                        return false;
                    }
                }
            }
        }

        std::vector<uint8_t> localBuffer;
        // If not using cache or not found in cache, proceed to load from file
        if (!loadFileToBuffer(filePath, localBuffer)) {
            LOG_ERROR("Image", "Failed to load file into buffer: " + filePath);
            return false;
        }

        SDL_RWops* rw = SDL_RWFromConstMem(localBuffer.data(), static_cast<int>(localBuffer.size()));
        if (!rw) {
            LOG_ERROR("Image", "Failed to create RWops from buffer: " + std::string(SDL_GetError()));
            return false;
        }

        CachedImage newCachedImage;
        bool isAnimated = false;
        bool animatedGif = isAnimatedGIF(localBuffer);
        bool animatedWebP = isAnimatedWebP(localBuffer);

        if (animatedWebP) {
            IMG_Animation* animation = IMG_LoadWEBPAnimation_RW(rw);
            if (!animation) {
                LOG_ERROR("Image", "Failed to load WebP animation: " + std::string(IMG_GetError()));
                SDL_RWclose(rw);
                return false;
            }

            WebPData webpData = { localBuffer.data(), localBuffer.size() };
            WebPDemuxer* demux = WebPDemux(&webpData);
            if (!demux) {
                LOG_ERROR("Image", "Failed to initialize WebP demuxer.");
                IMG_FreeAnimation(animation);
                SDL_RWclose(rw);
                return false;
            }

            WebPIterator iter;
            if (!WebPDemuxGetFrame(demux, 1, &iter)) {
                LOG_ERROR("Image", "Failed to get frames for WebP animation.");
                WebPDemuxDelete(demux);
                IMG_FreeAnimation(animation);
                SDL_RWclose(rw);
                return false;
            }

            int maxWidth = animation->w;
            int maxHeight = animation->h;

            SDL_LockMutex(SDL::getMutex());

            SDL_Surface* canvasSurface = SDL_CreateRGBSurfaceWithFormat(0, maxWidth, maxHeight, 32, SDL_PIXELFORMAT_RGBA32);
            if (!canvasSurface) {
                LOG_ERROR("Image", "Failed to create canvas surface for WebP animation.");
                WebPDemuxReleaseIterator(&iter);
                WebPDemuxDelete(demux);
                IMG_FreeAnimation(animation);
                SDL_UnlockMutex(SDL::getMutex());
                SDL_RWclose(rw);
                return false;
            }

            SDL_FillRect(canvasSurface, nullptr, SDL_MapRGBA(canvasSurface->format, 0, 0, 0, 0));

            std::vector<SDL_Texture*>& targetTextures = useTextureCaching_ ?
                newCachedImage.frameTextures : frameTextures_;

            do {
                SDL_Surface* frameSurface = SDL_CreateRGBSurfaceWithFormat(0, iter.width, iter.height, 32, SDL_PIXELFORMAT_RGBA32);
                if (!frameSurface) {
                    LOG_ERROR("Image", "Failed to create surface for WebP frame: " + std::to_string(iter.frame_num));
                    continue;
                }

                uint8_t const* ret = WebPDecodeRGBAInto(iter.fragment.bytes, iter.fragment.size,
                    (uint8_t*)frameSurface->pixels, frameSurface->pitch * frameSurface->h, frameSurface->pitch);
                if (!ret) {
                    LOG_ERROR("Image", "Failed to decode WebP frame: " + std::to_string(iter.frame_num));
                    SDL_FreeSurface(frameSurface);
                    continue;
                }

                SDL_Rect frameRect = { iter.x_offset, iter.y_offset, iter.width, iter.height };

                if (iter.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND) {
                    SDL_FillRect(canvasSurface, &frameRect, SDL_MapRGBA(canvasSurface->format, 0, 0, 0, 0));
                }

                SDL_SetSurfaceBlendMode(frameSurface, SDL_BLENDMODE_NONE);
                if (SDL_BlitSurface(frameSurface, nullptr, canvasSurface, &frameRect) != 0) {
                    LOG_ERROR("Image", "Failed to blit WebP frame onto canvas: " + std::string(SDL_GetError()));
                    SDL_FreeSurface(frameSurface);
                    continue;
                }

                SDL_FreeSurface(frameSurface);

                SDL_Texture* frameTexture = SDL_CreateTextureFromSurface(SDL::getRenderer(baseViewInfo.Monitor), canvasSurface);
                if (frameTexture) {
                    SDL_SetTextureBlendMode(frameTexture, SDL_BLENDMODE_BLEND);
                    targetTextures.push_back(frameTexture);
                }
                else {
                    LOG_ERROR("Image", "Failed to create texture from WebP frame: " + std::string(SDL_GetError()));
                }

                LOG_INFO("Image", "Processing frame " + std::to_string(iter.frame_num) + ": offset (" +
                    std::to_string(iter.x_offset) + ", " + std::to_string(iter.y_offset) + "), size (" +
                    std::to_string(iter.width) + "x" + std::to_string(iter.height) + ")");
            } while (WebPDemuxNextFrame(&iter));

            SDL_FreeSurface(canvasSurface);
            SDL_UnlockMutex(SDL::getMutex());
            WebPDemuxReleaseIterator(&iter);
            WebPDemuxDelete(demux);

            if (targetTextures.empty()) {
                LOG_ERROR("Image", "No frame textures were created for WebP animated image.");
                IMG_FreeAnimation(animation);
                SDL_RWclose(rw);
                return false;
            }

            baseViewInfo.ImageWidth = static_cast<float>(maxWidth);
            baseViewInfo.ImageHeight = static_cast<float>(maxHeight);
            lastFrameTime_ = SDL_GetTicks();

            if (useTextureCaching_) {
                newCachedImage.frameDelay = iter.duration;
            }
            else {
                frameDelay_ = iter.duration;
            }

            IMG_FreeAnimation(animation);
            SDL_RWclose(rw);
            isAnimated = true;

            LOG_INFO("Image", "Loaded WebP animated texture.");
        }
else if (animatedGif) {
    IMG_Animation* animation = IMG_LoadAnimation_RW(rw, 0);
    if (!animation) {
        LOG_ERROR("Image", "Failed to load GIF animation: " + std::string(IMG_GetError()));
        SDL_RWclose(rw);
        return false;
    }

    // Validate animation data
    if (animation->count <= 0 || !animation->frames || !animation->delays) {
        LOG_ERROR("Image", "Invalid GIF animation data");
        IMG_FreeAnimation(animation);
        SDL_RWclose(rw);
        return false;
    }

    SDL_LockMutex(SDL::getMutex());

    std::vector<SDL_Texture*>& targetTextures = useTextureCaching_ ?
        newCachedImage.frameTextures : frameTextures_;

    bool hasError = false;
    for (int i = 0; i < animation->count; ++i) {
        if (!animation->frames[i]) {
            LOG_ERROR("Image", "Invalid frame at index " + std::to_string(i));
            continue;
        }

        SDL_Texture* frameTexture = SDL_CreateTextureFromSurface(
            SDL::getRenderer(baseViewInfo.Monitor),
            animation->frames[i]
        );

        if (frameTexture) {
            SDL_SetTextureBlendMode(frameTexture, SDL_BLENDMODE_BLEND);  // Added blend mode setting
            targetTextures.push_back(frameTexture);
        }
        else {
            LOG_ERROR("Image", "Failed to create texture from GIF animation frame " +
                std::to_string(i) + ": " + std::string(SDL_GetError()));
            hasError = true;
        }
    }

    SDL_UnlockMutex(SDL::getMutex());

    if (targetTextures.empty()) {
        LOG_ERROR("Image", "No frame textures were created for GIF animated image: " + filePath);
        IMG_FreeAnimation(animation);
        SDL_RWclose(rw);
        return false;
    }

    // Set dimensions before potential early returns
    baseViewInfo.ImageWidth = static_cast<float>(animation->w);
    baseViewInfo.ImageHeight = static_cast<float>(animation->h);

    // Set timing information
    if (animation->delays && animation->count > 0) {
        if (useTextureCaching_) {
            newCachedImage.frameDelay = animation->delays[0];
        }
        else {
            frameDelay_ = animation->delays[0];
        }
    }
    else {
        // Default delay if none provided
        int defaultDelay = 100; // 100ms default
        if (useTextureCaching_) {
            newCachedImage.frameDelay = defaultDelay;
        }
        else {
            frameDelay_ = defaultDelay;
        }
        LOG_WARNING("Image", "No delay information found, using default delay: " + filePath);
    }

    lastFrameTime_ = SDL_GetTicks();
    isAnimated = true;

    // Log warning if some frames failed but we still have valid textures
    if (hasError && !targetTextures.empty()) {
        LOG_WARNING("Image", "Loaded GIF animation with some frame errors: " + filePath);
    }
    else {
        LOG_INFO("Image", "Loaded GIF animated texture: " + filePath);
    }

    IMG_FreeAnimation(animation);
}
        // Handle static images
        if (!isAnimated) {
            SDL_Texture* newTexture = IMG_LoadTexture_RW(SDL::getRenderer(baseViewInfo.Monitor), rw, 0);
            if (newTexture) {
                SDL_SetTextureBlendMode(newTexture, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                int width, height;
                if (SDL_QueryTexture(newTexture, nullptr, nullptr, &width, &height) == 0) {
                    baseViewInfo.ImageWidth = static_cast<float>(width);
                    baseViewInfo.ImageHeight = static_cast<float>(height);

                    if (useTextureCaching_) {
                        newCachedImage.texture = newTexture;
                    } else {
                        texture_ = newTexture;
                    }

                    LOG_INFO("Image", "Loaded static texture: " + filePath);
                }
            }
        }

        SDL_RWclose(rw);

        // Add to cache only if caching is enabled
        if (useTextureCaching_) {
            std::unique_lock<std::shared_mutex> lock(textureCacheMutex_);
            textureCache_[cacheKey] = std::move(newCachedImage);
            const CachedImage& cachedImage = textureCache_.at(cacheKey);

            if (cachedImage.texture) {
                texture_ = cachedImage.texture;
            }
            if (isAnimated) {
                // Simple vector copy instead of pointer allocation
                frameTextures_ = cachedImage.frameTextures;
                frameDelay_ = cachedImage.frameDelay;
            }
        }

        LOG_INFO("Image", "Loaded " << (useTextureCaching_ ? "and cached " : "without caching ") << "texture: " << filePath);

        return true;
        };

    // Attempt to load the primary file
    if (tryLoad(file_)) {
        return;
    }

    // If primary file failed, attempt to load the alternative file
    if (!altFile_.empty()) {
        if (tryLoad(altFile_)) {
            return;
        }
    }

    LOG_ERROR("Image", "Failed to load both primary and alternative image files: " + file_ + " | " + altFile_);
}

void Image::draw() {
    Component::draw();

    // Calculate the destination rectangle for rendering
    SDL_FRect rect = { baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(), baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };

    // Prioritize static image rendering
    if (texture_) {
        SDL_LockMutex(SDL::getMutex());

        if (!SDL::renderCopyF(texture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
            page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
            page.getLayoutHeightByMonitor(baseViewInfo.Monitor))) {
            LOG_ERROR("Image", "Failed to render static texture.");
        }

        SDL_UnlockMutex(SDL::getMutex());
    }
    else if (frameDelay_ != 0 && !frameTextures_.empty()) {
        Uint32 currentTime = SDL_GetTicks();
        Uint32 elapsed = currentTime - lastFrameTime_;

        // Handle timer wraparound
        if (elapsed >= static_cast<Uint32>(frameDelay_)) {
            size_t framesToAdvance = elapsed / frameDelay_;
            currentFrame_ = (currentFrame_ + framesToAdvance) % frameTextures_.size();
            lastFrameTime_ = currentTime - (elapsed % frameDelay_);
        }

        // Render the current animation frame if valid
        SDL_Texture* frameTexture = frameTextures_[currentFrame_];
        if (frameTexture) {
            SDL_LockMutex(SDL::getMutex());

            if (!SDL::renderCopyF(frameTexture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
                page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
                page.getLayoutHeightByMonitor(baseViewInfo.Monitor))) {
                LOG_ERROR("Image", "Failed to render animation frame.");
            }

            SDL_UnlockMutex(SDL::getMutex());
        }
        else {
            LOG_ERROR("Image", "Frame texture is null before rendering frame: " + std::to_string(currentFrame_));
        }
    }
    else {
        LOG_ERROR("Image", "No valid texture or animation to draw.");
    }
}

std::string_view Image::filePath() {
	return file_;
}

// Static method to clean up the texture cache
void Image::cleanupTextureCache() {
    std::unique_lock<std::shared_mutex> lock(textureCacheMutex_);
    for (auto& pair : textureCache_) {

        // Destroy static textures
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
            pair.second.texture = nullptr;
        }

        // Destroy frame textures for animated images
        if (!pair.second.frameTextures.empty()) {
            // Lock the rendering mutex before destroying frame textures
            SDL_LockMutex(SDL::getMutex());

            // Free each frame texture
            for (SDL_Texture* frameTexture : pair.second.frameTextures) {
                if (frameTexture) {
                    SDL_DestroyTexture(frameTexture);
                }
            }

            // Unlock the rendering mutex after destroying frame textures
            SDL_UnlockMutex(SDL::getMutex());

            // Clear the frame texture vector
            pair.second.frameTextures.clear();

            // Reset the frame delay to 0
            pair.second.frameDelay = 0;

            LOG_INFO("TextureCache", "Destroyed cached animated textures");
        }
    }

    // Clear the entire texture cache
    textureCache_.clear();
    LOG_INFO("TextureCache", "All cached textures have been destroyed.");
}
