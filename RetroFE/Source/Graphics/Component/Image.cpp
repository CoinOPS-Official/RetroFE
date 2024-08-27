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
#include "../../SDL.h"
#include "../../Utility/Log.h"
#if (__APPLE__)
    #include <SDL2_image/SDL_image.h>
#else
    #include <SDL2/SDL_image.h>
#endif
#include <string_view>

Image::Image(const std::string& file, const std::string& altFile, Page &p, int monitor, bool additive)
    : Component(p), file_(file), altFile_(altFile)
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

    SDL_LockMutex(SDL::getMutex());

    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    if (!frameTextures_.empty()) {
        for (SDL_Texture* frameTexture : frameTextures_) {
            SDL_DestroyTexture(frameTexture);
        }
        frameTextures_.clear();
    }

    if (animation_ != nullptr) {
        IMG_FreeAnimation(animation_);
        animation_ = nullptr;
    }

    SDL_UnlockMutex(SDL::getMutex());
}

void Image::allocateGraphicsMemory() {
    if (!texture_ && frameTextures_.empty()) {

        auto endsWith = [](std::string_view str, std::string_view suffix) {
            return str.size() >= suffix.size() &&
                str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
            };

        std::string_view fileView = file_;
        std::string_view altFileView = altFile_;
        bool isGif = endsWith(fileView, ".gif") || (!altFile_.empty() && endsWith(altFileView, ".gif"));

        if (isGif) {
            // Try to load the GIF as an animation
            animation_ = IMG_LoadAnimation(file_.c_str());
            if (!animation_ && !altFile_.empty()) {
                animation_ = IMG_LoadAnimation(altFile_.c_str());
            }

            if (animation_) {
                // Preload all frames as textures
                for (int i = 0; i < animation_->count; ++i) {
                    SDL_Texture* frameTexture = SDL_CreateTextureFromSurface(SDL::getRenderer(baseViewInfo.Monitor), animation_->frames[i]);
                    if (frameTexture) {
                        frameTextures_.push_back(frameTexture);
                    }
                }
                baseViewInfo.ImageWidth = static_cast<float>(animation_->w);
                baseViewInfo.ImageHeight = static_cast<float>(animation_->h);
                lastFrameTime_ = SDL_GetTicks();
            }
            else {
                LOG_ERROR("Image", "Failed to load GIF animation: " + std::string(IMG_GetError()));
            }
        }

        // If not a GIF, or GIF loading failed, treat as a static image
        if (!animation_) {
            texture_ = IMG_LoadTexture(SDL::getRenderer(baseViewInfo.Monitor), file_.c_str());
            if (!texture_ && !altFile_.empty()) {
                texture_ = IMG_LoadTexture(SDL::getRenderer(baseViewInfo.Monitor), altFile_.c_str());
            }

            if (texture_) {
                int width, height;

                // Set the blend mode
                SDL_SetTextureBlendMode(texture_, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

                SDL_QueryTexture(texture_, nullptr, nullptr, &width, &height);
                baseViewInfo.ImageWidth = static_cast<float>(width);
                baseViewInfo.ImageHeight = static_cast<float>(height);
            }
            else {
                LOG_ERROR("Image", "Failed to load image: " + std::string(IMG_GetError()));
            }
        }
    }

    // Call the base class implementation
    Component::allocateGraphicsMemory();
}

void Image::draw() {
    Component::draw();

    if (baseViewInfo.Alpha > 0.0f) {
        SDL_Rect rect = { 0, 0, 0, 0 };
        rect.x = static_cast<int>(baseViewInfo.XRelativeToOrigin());
        rect.y = static_cast<int>(baseViewInfo.YRelativeToOrigin());
        rect.h = static_cast<int>(baseViewInfo.ScaledHeight());
        rect.w = static_cast<int>(baseViewInfo.ScaledWidth());

        // Prioritize static image rendering
        if (texture_) {
            SDL::renderCopy(texture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
        } else if (!frameTextures_.empty()) {
            // Handle animation rendering
            Uint32 currentTime = SDL_GetTicks();
            if (currentTime - lastFrameTime_ > static_cast<Uint32>(animation_->delays[currentFrame_])) {
                currentFrame_ = (currentFrame_ + 1) % animation_->count;
                lastFrameTime_ = currentTime;
            }

            SDL_Texture* frameTexture = frameTextures_[currentFrame_];
            SDL::renderCopy(frameTexture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
        }
    }
}


std::string_view Image::filePath() {
    return file_;
}