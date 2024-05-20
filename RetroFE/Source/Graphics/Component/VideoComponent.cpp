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

#include "VideoComponent.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../Video/VideoFactory.h"
#include "../../SDL.h"
#include "../Page.h"
#include "SDL_rect.h"
#include "SDL_render.h"

VideoComponent::VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops)
    : Component(p), videoFile_(videoFile), videoInst_(nullptr), isPlaying_(false),
    hasBeenOnScreen_(false), numLoops_(numLoops), monitor_(monitor), currentPage_(&p),
    videoMutex_(SDL_CreateMutex())
{
    if (!videoMutex_) {
        LOG_ERROR("VideoComponent", "Failed to create mutex");
    }
}

VideoComponent::~VideoComponent()
{
    VideoComponent::freeGraphicsMemory();
    if (videoMutex_) {
        SDL_DestroyMutex(videoMutex_);
    }
}

bool VideoComponent::update(float dt)
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            isPlaying_ = videoInst_->isPlaying();
        }

        if (videoInst_ && isPlaying_) {
            videoInst_->setVolume(baseViewInfo.Volume);
            videoInst_->update(dt);
            videoInst_->volumeUpdate();
            if (!currentPage_->isMenuScrolling()) {
                videoInst_->loopHandler();
            }

            if (baseViewInfo.ImageHeight == 0 && baseViewInfo.ImageWidth == 0) {
                baseViewInfo.ImageHeight = static_cast<float>(videoInst_->getHeight());
                baseViewInfo.ImageWidth = static_cast<float>(videoInst_->getWidth());
            }

            bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0;

            if (isCurrentlyVisible) {
                hasBeenOnScreen_ = true;
            }

            if (baseViewInfo.PauseOnScroll && !currentPage_->isMenuFastScrolling()) {
                if (!isCurrentlyVisible && !isPaused()) {
                    videoInst_->pause();
                    if (Logger::isLevelEnabled("DEBUG")) {
                        LOG_DEBUG("VideoComponent", "Paused " + Utils::getFileName(videoFile_));
                    }
                }
                else if (isCurrentlyVisible && isPaused()) {
                    videoInst_->pause();
                    if (Logger::isLevelEnabled("DEBUG")) {
                        LOG_DEBUG("VideoComponent", "Resumed " + Utils::getFileName(videoFile_));
                    }
                }
            }

            if (baseViewInfo.Restart && hasBeenOnScreen_) {
                videoInst_->restart();
                if (Logger::isLevelEnabled("DEBUG")) {
                    LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
                }
                baseViewInfo.Restart = false;
            }
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in update()");
    }

    return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        Component::allocateGraphicsMemory();

        if (!isPlaying_) {
            if (!videoInst_) {
                videoInst_ = VideoFactory::createVideo(monitor_, numLoops_);
                if (!videoInst_) {
                    LOG_ERROR("VideoComponent", "Failed to create video instance");
                }
            }
            if (!videoFile_.empty() && videoInst_) {
                isPlaying_ = videoInst_->play(videoFile_);
            }
        }

        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in allocateGraphicsMemory()");
    }
}

void VideoComponent::freeGraphicsMemory()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        Component::freeGraphicsMemory();
        if (Logger::isLevelEnabled("DEBUG")) {
            LOG_DEBUG("VideoComponent", "Component Freed " + Utils::getFileName(videoFile_));
        }

        if (videoInst_) {
            videoInst_->stop();
            delete videoInst_;
            videoInst_ = nullptr;
            isPlaying_ = false;
            if (Logger::isLevelEnabled("DEBUG")) {
                LOG_DEBUG("VideoComponent", "Deleted " + Utils::getFileName(videoFile_));
            }
        }

        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in freeGraphicsMemory()");
    }
}

void VideoComponent::draw()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (baseViewInfo.Alpha > 0.0f) {
            SDL_Rect rect = { 0, 0, 0, 0 };

            rect.x = static_cast<int>(baseViewInfo.XRelativeToOrigin());
            rect.y = static_cast<int>(baseViewInfo.YRelativeToOrigin());
            rect.h = static_cast<int>(baseViewInfo.ScaledHeight());
            rect.w = static_cast<int>(baseViewInfo.ScaledWidth());

            videoInst_->draw();
            SDL_Texture* texture = videoInst_->getTexture();

            if (texture) {
                SDL::renderCopy(texture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
            }
        }

        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in draw()");
    }
}

bool VideoComponent::isPlaying()
{
    bool playing = false;
    if (SDL_LockMutex(videoMutex_) == 0) {
        playing = isPlaying_;
        SDL_UnlockMutex(videoMutex_);
    }
    return playing;
}

std::string_view VideoComponent::filePath()
{
    return videoFile_;
}

void VideoComponent::skipForward()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            videoInst_->skipForward();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in skipForward()");
    }
}

void VideoComponent::skipBackward()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            videoInst_->skipBackward();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in skipBackward()");
    }
}

void VideoComponent::skipForwardp()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            videoInst_->skipForwardp();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in skipForwardp()");
    }
}

void VideoComponent::skipBackwardp()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            videoInst_->skipBackwardp();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in skipBackwardp()");
    }
}

void VideoComponent::pause()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            videoInst_->pause();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in pause()");
    }
}

void VideoComponent::restart()
{
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            videoInst_->restart();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    else {
        LOG_ERROR("VideoComponent", "Failed to lock mutex in restart()");
    }
}

unsigned long long VideoComponent::getCurrent()
{
    unsigned long long current = 0;
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            current = videoInst_->getCurrent();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    return current;
}

unsigned long long VideoComponent::getDuration()
{
    unsigned long long duration = 0;
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            duration = videoInst_->getDuration();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    return duration;
}

bool VideoComponent::isPaused()
{
    bool paused = false;
    if (SDL_LockMutex(videoMutex_) == 0) {
        if (videoInst_) {
            paused = videoInst_->isPaused();
        }
        SDL_UnlockMutex(videoMutex_);
    }
    return paused;
}