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

#ifdef __APPLE__
#include <SDL2_image/SDL_image.h>
#include <webp/decode.h>
#include <webp/demux.h>
#elif defined(_WIN32) 
#include <SDL_image.h>
#include <decode.h>
#include <demux.h>
#else  // Assume Linux
#include <SDL2/SDL_image.h>
#include <webp/decode.h>
#include <webp/demux.h>
#endif

#include <string_view>
#include <fstream>
#include <vector>
#include <cstring>
#include <optional>

 // Definition of static members
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::shared_mutex Image::textureCacheMutex_;

//
// In this revision, we store animated frames as surfaces (instead of textures) in animatedSurfaces_.
// During draw(), we update a single texture using these surfaces.
//

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
    // Process file path using a string_view to avoid unnecessary copies.
    std::string_view pathView(filePath);
    size_t lastSlash = pathView.find_last_of("\\/");
    std::string_view directoryView = (lastSlash != std::string_view::npos) ?
        pathView.substr(0, lastSlash) : std::string_view(".");
    std::string_view filenameView = (lastSlash != std::string_view::npos) ?
        pathView.substr(lastSlash + 1) : pathView;
    std::scoped_lock<std::mutex> lock(cacheMutex_);
    return {
        *directories_.emplace(directoryView).first,
        *filenames_.emplace(filenameView).first,
        monitor
    };
}

Image::Image(const std::string& file, const std::string& altFile, Page& p, int monitor, bool additive, bool useTextureCaching)
    : Component(p), file_(file), altFile_(altFile), useTextureCaching_(useTextureCaching)
{
    baseViewInfo.Monitor = monitor;
    baseViewInfo.Additive = additive;
    baseViewInfo.Layout = page.getCurrentLayout();
}

Image::~Image() {
    freeGraphicsMemory();
}

void Image::allocateGraphicsMemory() {
    // If a static texture is already present or animated surfaces have been loaded (or referenced), skip reloading.
    if (texture_ || !animatedSurfaces_.empty()) return;

    auto tryLoad = [this](const std::string& filePath) -> bool {
        auto cacheKey = pathCache_.getKey(filePath, baseViewInfo.Monitor);
        CachedImage newCachedImage;

        LoadContext ctx{
            filePath,
            cacheKey,
            newCachedImage,
            baseViewInfo,
            animatedSurfaces_, // this is where we want to store surfaces when not caching,
            frameDelay_,
            lastFrameTime_,
            useTextureCaching_
        };

        // Check the cache first.
        if (loadFromCache(ctx)) return true;

        // Loading file into the buffer
        std::vector<uint8_t> buffer;
        if (!loadFileToBuffer(filePath, buffer))
            return false;

        bool success = false;
        // Check for WebP header.
        if (buffer.size() >= 12 && std::memcmp(buffer.data(), "RIFF", 4) == 0 &&
            std::memcmp(buffer.data() + 8, "WEBP", 4) == 0) {
            if (isAnimatedWebP(buffer))
                success = loadAnimatedWebP(buffer, ctx);
            else
                success = loadStaticImage(buffer, ctx);
        }
        // Check for GIF header.
        else if (buffer.size() >= 6 && (std::memcmp(buffer.data(), "GIF87a", 6) == 0 ||
            std::memcmp(buffer.data(), "GIF89a", 6) == 0)) {
            if (isAnimatedGIF(buffer))
                success = loadAnimatedGIF(buffer, ctx);
            else
                success = loadStaticImage(buffer, ctx);
        }
        else {
            success = loadStaticImage(buffer, ctx);
        }

        if (success && useTextureCaching_) {
            // For static images the texture is saved to the cache.
            if (newCachedImage.texture) {
                texture_ = newCachedImage.texture;
            }
            // For animated images, store the decoded surfaces in the cache.
            if (!newCachedImage.animatedSurfaces.empty()) {
                animatedSurfaces_ = newCachedImage.animatedSurfaces;  // assign pointers; cache remains owner
                frameDelay_ = newCachedImage.frameDelay;
                isUsingCachedSurfaces_ = true;
            }
            std::unique_lock<std::shared_mutex> lock(textureCacheMutex_);
            textureCache_[cacheKey] = std::move(newCachedImage);
        }
        return success;
        };

    if (tryLoad(file_))
        return;
    if (!altFile_.empty() && tryLoad(altFile_))
        return;
    LOG_ERROR("Image", "Failed to load both primary and alternative image files: " + file_ + " | " + altFile_);
}

void Image::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    // For static images.
    if (frameDelay_ == 0) {
        if (!useTextureCaching_) {
            // When caching is disabled, destroy the texture.
            if (texture_) {
                SDL_DestroyTexture(texture_);
            }
        }
        // Always reset the instance pointer.
        texture_ = nullptr;
    }
    // For animated images.
    else {
        if (!useTextureCaching_) {
            // When caching is disabled, destroy the animated texture.
            if (animatedTexture_) {
                SDL_DestroyTexture(animatedTexture_);
            }
        }
        // Always reset the instance pointer.
        animatedTexture_ = nullptr;
    }

    // For animated surfaces, free them only if caching is disabled.
    if (frameDelay_ != 0 && !useTextureCaching_) {
        for (SDL_Surface* surf : animatedSurfaces_) {
            if (surf) {
                SDL_FreeSurface(surf);
            }
        }
    }
    // Always clear the instance's copy of the surfaces.
    animatedSurfaces_.clear();
}

void Image::draw() {
    Component::draw();

    SDL_FRect rect = {
        baseViewInfo.XRelativeToOrigin(),
        baseViewInfo.YRelativeToOrigin(),
        baseViewInfo.ScaledWidth(),
        baseViewInfo.ScaledHeight()
    };

    if (frameDelay_ != 0) {
        // Animated image: both animatedSurfaces_ and animatedTexture_ must be valid.
        if (animatedSurfaces_.empty() || !animatedTexture_) {
            LOG_ERROR("Image", "Animated image resources are missing. Cannot draw animated image.");
            return;
        }
        Uint32 currentTime = SDL_GetTicks();
        Uint32 elapsed = currentTime - lastFrameTime_;
        if (elapsed >= static_cast<Uint32>(frameDelay_)) {
            size_t framesToAdvance = elapsed / frameDelay_;
            currentFrame_ = (currentFrame_ + framesToAdvance) % animatedSurfaces_.size();
            lastFrameTime_ = currentTime - (elapsed % frameDelay_);
        }
        SDL_Surface* currentSurface = animatedSurfaces_[currentFrame_];
        if (!currentSurface) {
            LOG_ERROR("Image", "Current animated surface is null (frame index: " + std::to_string(currentFrame_) + ")");
            return;
        }
        SDL_LockMutex(SDL::getMutex());
        // Rely on the cached animated texture without creating a new one.
        if (SDL_UpdateTexture(animatedTexture_, nullptr, currentSurface->pixels, currentSurface->pitch) != 0) {
            LOG_ERROR("Image", "Failed to update animated texture: " + std::string(SDL_GetError()));
            SDL_UnlockMutex(SDL::getMutex());
        }
        SDL_UnlockMutex(SDL::getMutex());
    }

    // For static images, texture_ is used; for animated images, animatedTexture_ is used.
    SDL_Texture* textureToRender = (frameDelay_ == 0) ? texture_ : animatedTexture_;
    if (!textureToRender) {
        LOG_ERROR("Image", "No valid texture (static or animated) to draw.");
        return;
    }
    if (!SDL::renderCopyF(textureToRender, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor))) {
        LOG_ERROR("Image", "Failed to render texture.");
    }
}

std::string_view Image::filePath() {
    return file_;
}

void Image::cleanupTextureCache() {
    std::unique_lock<std::shared_mutex> lock(textureCacheMutex_);
    for (auto& pair : textureCache_) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
            pair.second.texture = nullptr;
        }
        if (!pair.second.animatedSurfaces.empty()) {
            for (SDL_Surface* surf : pair.second.animatedSurfaces) {
                if (surf) SDL_FreeSurface(surf);
            }
            pair.second.animatedSurfaces.clear();
            pair.second.frameDelay = 0;
            LOG_INFO("TextureCache", "Destroyed cached animated surfaces");
        }
    }
    textureCache_.clear();
    LOG_INFO("TextureCache", "All cached textures and animated surfaces have been destroyed.");
}

bool Image::loadFromCache(const LoadContext& ctx) {
    if (!ctx.useCache) {
        LOG_INFO("Image", "Caching is disabled. Skipping cache load for: " + ctx.filePath);
        return false;
    }

    // Use a shared lock for reading.
    std::shared_lock<std::shared_mutex> lock(textureCacheMutex_);
    LOG_INFO("Image", "Attempting to locate cache entry for key associated with: " + ctx.filePath);

    auto it = textureCache_.find(ctx.cacheKey);
    if (it == textureCache_.end()) {
        LOG_INFO("Image", "Cache miss for: " + ctx.filePath);
        return false;
    }

    CachedImage& cachedImage = it->second;
    bool validCacheEntry = false;


        if (cachedImage.texture) {
            int width, height;
            if (SDL_QueryTexture(cachedImage.texture, nullptr, nullptr, &width, &height) == 0) {
                validCacheEntry = true;
                texture_ = cachedImage.texture;
                ctx.baseViewInfo.ImageWidth = static_cast<float>(width);
                ctx.baseViewInfo.ImageHeight = static_cast<float>(height);
                LOG_INFO("Image", "Loaded static texture from cache for " + ctx.filePath +
                    " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
            }
            else {
                LOG_ERROR("Image", "Cached static texture is invalid for " + ctx.filePath +
                    ": " + std::string(SDL_GetError()));
            }
        }
    
    else {
        // Animated image: Validate animated surfaces and animated texture.
        if (!cachedImage.animatedSurfaces.empty() && cachedImage.animatedTexture) {
            // Validate surfaces.
            if (validateSurfaces(cachedImage.animatedSurfaces)) {
                // Retrieve the expected dimensions from the first surface.
                int surfW = cachedImage.animatedSurfaces[0]->w;
                int surfH = cachedImage.animatedSurfaces[0]->h;
                // Validate the animated texture dimensions.
                int texW, texH;
                if (SDL_QueryTexture(cachedImage.animatedTexture, nullptr, nullptr, &texW, &texH) == 0) {
                    if (texW == surfW && texH == surfH) {
                        validCacheEntry = true;
                        animatedSurfaces_ = cachedImage.animatedSurfaces;
                        animatedTexture_ = cachedImage.animatedTexture;
                        frameDelay_ = cachedImage.frameDelay;
                        ctx.baseViewInfo.ImageWidth = static_cast<float>(surfW);
                        ctx.baseViewInfo.ImageHeight = static_cast<float>(surfH);
                        lastFrameTime_ = SDL_GetTicks();
                        isUsingCachedSurfaces_ = true;
                        LOG_INFO("Image", "Loaded animated surfaces and texture from cache for " +
                            ctx.filePath + " (" + std::to_string(surfW) + "x" + std::to_string(surfH) + ")");
                    }
                    else {
                        LOG_ERROR("Image", "Animated texture dimensions (" +
                            std::to_string(texW) + "x" + std::to_string(texH) +
                            ") do not match animated surfaces (" +
                            std::to_string(surfW) + "x" + std::to_string(surfH) + ") for " + ctx.filePath);
                    }
                }
                else {
                    LOG_ERROR("Image", "Failed to query animated texture for " + ctx.filePath + ": " + std::string(SDL_GetError()));
                }
            }
            else {
                LOG_ERROR("Image", "Animated surfaces validation failed for " + ctx.filePath);
            }
        }
    }

    // If the cache entry is invalid, remove it.
    if (!validCacheEntry) {
        lock.unlock();  // Release shared lock before acquiring unique lock.
        std::unique_lock<std::shared_mutex> uniqueLock(textureCacheMutex_);
        textureCache_.erase(ctx.cacheKey);
        LOG_WARNING("Image", "Removed invalid cache entry for: " + ctx.filePath);
        return false;
    }
    return true;
}

bool Image::validateSurfaces(const std::vector<SDL_Surface*>& surfaces) const {
    if (surfaces.empty()) {
        return false;
    }

    // Optional: use the first surface as a baseline for dimensions.
    int expectedWidth = surfaces[0]->w;
    int expectedHeight = surfaces[0]->h;

    for (SDL_Surface* surf : surfaces) {
        if (!surf) {
            LOG_ERROR("Image", "Surface pointer is null.");
            return false;
        }
        if (!surf->pixels) {
            LOG_ERROR("Image", "Surface pixels pointer is null.");
            return false;
        }
        if (surf->w <= 0 || surf->h <= 0) {
            LOG_ERROR("Image", "Surface has invalid dimensions (" + std::to_string(surf->w) + "x" + std::to_string(surf->h) + ").");
            return false;
        }
        // Optionally, ensure all surfaces are the same size.
        if (surf->w != expectedWidth || surf->h != expectedHeight) {
            LOG_ERROR("Image", "Animated surfaces have inconsistent dimensions.");
            return false;
        }
        // Optionally, check pixel format (e.g., expecting 32-bit RGBA)
        if (surf->format->BytesPerPixel != 4) {
            LOG_ERROR("Image", "Surface pixel format is not 32-bit as expected.");
            return false;
        }
    }
    return true;
}


bool Image::loadFileToBuffer(const std::string& filePath, std::vector<uint8_t>& outBuffer) {
    LOG_INFO("Image", "Attempting to load file into buffer: " + filePath);
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR("Image", "Failed to open file: " + filePath);
        return false;
    }
    std::streamsize size = file.tellg();
    LOG_INFO("Image", "File size: " + std::to_string(size) + " bytes");
    if (size <= 0) {
        LOG_ERROR("Image", "File is empty or invalid: " + filePath);
        return false;
    }
    file.seekg(0, std::ios::beg);
    outBuffer.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(outBuffer.data()), size)) {
        LOG_ERROR("Image", "Failed to read file: " + filePath);
        outBuffer.clear();
        return false;
    }
    LOG_INFO("Image", "Successfully loaded file into buffer: " + filePath);
    return true;
}

bool Image::loadStaticImage(const std::vector<uint8_t>& buffer, LoadContext& ctx) {
    SDL_RWops* rw = SDL_RWFromConstMem(buffer.data(), static_cast<int>(buffer.size()));
    if (!rw) {
        LOG_ERROR("Image", "Failed to create RWops from buffer: " + std::string(SDL_GetError()));
        return false;
    }
    SDL_Texture* newTex = IMG_LoadTexture_RW(SDL::getRenderer(baseViewInfo.Monitor), rw, 0);
    SDL_RWclose(rw);
    if (!newTex) {
        LOG_ERROR("Image", "Failed to load static texture: " + std::string(IMG_GetError()));
        return false;
    }
    SDL_SetTextureBlendMode(newTex, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
    int width, height;
    if (SDL_QueryTexture(newTex, nullptr, nullptr, &width, &height) == 0) {
        ctx.baseViewInfo.ImageWidth = static_cast<float>(width);
        ctx.baseViewInfo.ImageHeight = static_cast<float>(height);
        if (ctx.useCache) {
            ctx.newCachedImage.texture = newTex;
                texture_ = newTex; // Also update the instance's texture pointer
        }
        else {
            texture_ = newTex;
        }
        LOG_INFO("Image", "Loaded static texture: " + ctx.filePath);
        return true;
    }
    SDL_DestroyTexture(newTex);
    return false;
}

bool Image::loadAnimatedGIF(const std::vector<uint8_t>& buffer, LoadContext& ctx) {
    SDL_RWops* rw = SDL_RWFromConstMem(buffer.data(), static_cast<int>(buffer.size()));
    if (!rw) {
        LOG_ERROR("Image", "Failed to create RWops from buffer: " + std::string(SDL_GetError()));
        return false;
    }
    IMG_Animation* animation = IMG_LoadAnimation_RW(rw, 0);
    SDL_RWclose(rw);
    if (!animation || animation->count <= 0 || !animation->frames || !animation->delays) {
        LOG_ERROR("Image", "Invalid GIF animation data");
        if (animation) IMG_FreeAnimation(animation);
        return false;
    }

    // Prepare to store surfaces.
    // We'll use a temporary vector for loading the frames.
    std::vector<SDL_Surface*> decodedSurfaces;
    bool success = false;
    for (int i = 0; i < animation->count; ++i) {
        if (!animation->frames[i]) {
            LOG_ERROR("Image", "Invalid frame at index " + std::to_string(i));
            continue;
        }
        SDL_Surface* frameSurface = SDL_ConvertSurface(animation->frames[i], animation->frames[i]->format, 0);
        if (frameSurface) {
            decodedSurfaces.push_back(frameSurface);
            success = true;
        }
        else {
            LOG_ERROR("Image", "Failed to create surface from GIF frame " + std::to_string(i) + ": " + std::string(SDL_GetError()));
        }
    }

    if (success) {
        ctx.baseViewInfo.ImageWidth = static_cast<float>(animation->w);
        ctx.baseViewInfo.ImageHeight = static_cast<float>(animation->h);
        int delay = (animation->delays && animation->count > 0) ? animation->delays[0] : 100;
        if (ctx.useCache) {
            ctx.newCachedImage.frameDelay = delay;
            }
        else {
            frameDelay_ = delay;
            }
        lastFrameTime_ = SDL_GetTicks();
        LOG_INFO("Image", "Loaded animated GIF with " + std::to_string(decodedSurfaces.size()) + " frames");

        // Update the instance's animated surfaces regardless of caching.
        animatedSurfaces_ = decodedSurfaces;

        // Create the animated texture from the first frame, regardless of caching.
        if (!decodedSurfaces.empty()) {
            SDL_Surface* firstSurface = decodedSurfaces[0];
            SDL_Texture* animTex = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor),
                firstSurface->format->format,
                SDL_TEXTUREACCESS_STREAMING,
                firstSurface->w, firstSurface->h);
            if (animTex) {
                SDL_SetTextureBlendMode(animTex, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                // Always update the instance's animatedTexture_
                animatedTexture_ = animTex;
                // If caching is enabled, update the cache entry as well.
                if (ctx.useCache) {
                    ctx.newCachedImage.animatedSurfaces = decodedSurfaces;
                    ctx.newCachedImage.animatedTexture = animTex;
                    }
                }
            else {
                LOG_ERROR("Image", "Failed to create animated texture: " + std::string(SDL_GetError()));
                }
            }
        }

    IMG_FreeAnimation(animation);
    return success;
}


bool Image::loadAnimatedWebP(const std::vector<uint8_t>& buffer, LoadContext& ctx) {
    WebPData webpData = { buffer.data(), buffer.size() };
    WebPDemuxer* demux = WebPDemux(&webpData);
    if (!demux) {
        LOG_ERROR("Image", "Failed to initialize WebP demuxer.");
        return false;
        }
    uint32_t width = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    uint32_t height = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    uint32_t frameCount = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
    SDL_Surface* canvasSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!canvasSurface) {
        LOG_ERROR("Image", "Failed to create canvas surface for WebP animation.");
        WebPDemuxDelete(demux);
        return false;
        }
    SDL_FillRect(canvasSurface, nullptr, SDL_MapRGBA(canvasSurface->format, 0, 0, 0, 0));
    std::vector<SDL_Surface*> decodedSurfaces;
    decodedSurfaces.reserve(frameCount);
    WebPIterator iter;
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        int previousDispose = WEBP_MUX_DISPOSE_NONE;
        SDL_Rect previousRect = { 0, 0, 0, 0 };
        do {
            if (previousDispose == WEBP_MUX_DISPOSE_BACKGROUND) {
                SDL_FillRect(canvasSurface, &previousRect, SDL_MapRGBA(canvasSurface->format, 0, 0, 0, 0));
                }
            SDL_Surface* frameSurface = SDL_CreateRGBSurfaceWithFormat(0, iter.width, iter.height, 32, SDL_PIXELFORMAT_RGBA32);
            if (!frameSurface)
                continue;
            if (WebPDecodeRGBAInto(iter.fragment.bytes, iter.fragment.size,
                static_cast<uint8_t*>(frameSurface->pixels),
                frameSurface->pitch * frameSurface->h, frameSurface->pitch)) {
                SDL_Rect frameRect = { iter.x_offset, iter.y_offset, iter.width, iter.height };
                SDL_SetSurfaceBlendMode(frameSurface, iter.blend_method == WEBP_MUX_BLEND ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
                if (SDL_BlitSurface(frameSurface, nullptr, canvasSurface, &frameRect) == 0) {
                    SDL_Surface* frameCopy = SDL_ConvertSurface(canvasSurface, canvasSurface->format, 0);
                    if (frameCopy) {
                        decodedSurfaces.push_back(frameCopy);
                        }
                    }
                previousDispose = iter.dispose_method;
                previousRect = frameRect;
                }
            SDL_FreeSurface(frameSurface);
            } while (WebPDemuxNextFrame(&iter));
        // Set frame delay regardless of caching:
        int delay = (iter.duration > 0) ? iter.duration : 100;
        frameDelay_ = delay;
        if (ctx.useCache) {
            ctx.newCachedImage.frameDelay = delay;
            }
        WebPDemuxReleaseIterator(&iter);
        }
    SDL_FreeSurface(canvasSurface);
    WebPDemuxDelete(demux);

    if (decodedSurfaces.empty()) {
        LOG_ERROR("Image", "No frame surfaces were created for animated WebP image.");
        return false;
        }
    baseViewInfo.ImageWidth = static_cast<float>(width);
    baseViewInfo.ImageHeight = static_cast<float>(height);
    lastFrameTime_ = SDL_GetTicks();
    if (ctx.useCache) {
        LOG_INFO("Image", "Decoded animated WebP into " + std::to_string(decodedSurfaces.size()) +
            " surfaces (will be cached)");
        ctx.newCachedImage.animatedSurfaces = decodedSurfaces;
        animatedSurfaces_ = decodedSurfaces; // Also update the instance's member.
        // Create the animated texture from the first frame.
        SDL_Surface* firstSurface = decodedSurfaces[0];
        SDL_Texture* animTex = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor),
            firstSurface->format->format,
            SDL_TEXTUREACCESS_STREAMING,
            firstSurface->w, firstSurface->h);
        if (animTex) {
            SDL_SetTextureBlendMode(animTex, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
            ctx.newCachedImage.animatedTexture = animTex;
            animatedTexture_ = animTex;
            }
        else {
            LOG_ERROR("Image", "Failed to create animated texture from cached WebP frames: " +
                std::string(SDL_GetError()));
            }
        }
    else {
        animatedSurfaces_ = decodedSurfaces;
        LOG_INFO("Image", "Decoded animated WebP into " + std::to_string(animatedSurfaces_.size()) + " surfaces");
        // Create the animated texture for immediate drawing.
        SDL_Surface* firstSurface = decodedSurfaces[0];
        SDL_Texture* animTex = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor),
            firstSurface->format->format,
            SDL_TEXTUREACCESS_STREAMING,
            firstSurface->w, firstSurface->h);
        if (animTex) {
            SDL_SetTextureBlendMode(animTex, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
            animatedTexture_ = animTex;
            }
        else {
            LOG_ERROR("Image", "Failed to create animated texture from WebP frames: " + std::string(SDL_GetError()));
            }
        }
    return true;
}

bool Image::isAnimatedWebP(const std::vector<uint8_t>& buffer) {
    WebPData webpData = { buffer.data(), buffer.size() };
    WebPDemuxer* demux = WebPDemux(&webpData);
    if (!demux) {
        LOG_ERROR("Image", "Failed to initialize WebPDemuxer for animation check.");
        return false;
    }
    int frameCount = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
    WebPDemuxDelete(demux);
    return frameCount > 1;
}

bool Image::isAnimatedGIF(const std::vector<uint8_t>& buffer) {
    size_t frameCount = 0;
    for (size_t i = 0; i < buffer.size() - 1; ++i) {
        if (buffer[i] == 0x21 && buffer[i + 1] == 0xF9) {
            frameCount++;
            if (frameCount > 1) return true;
        }
    }
    return false;
}
