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

#include <string>
#include <type_traits>
#include <utility>

#include "../../Graphics/ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../Video/IVideo.h"
#include "../../Video/VideoFactory.h"
#include "../Page.h"
#include "SDL_rect.h"
#include "SDL_render.h"

VideoComponent::VideoComponent(Page &p, const std::string &videoFile, int monitor, int numLoops)
    : Component(p), videoFile_(videoFile), numLoops_(numLoops), monitor_(monitor), currentPage_(&p)
{
}

VideoComponent::~VideoComponent()
{
    VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt)
{
    if (!videoInst_ || !isPlaying_)
    {
        return Component::update(dt);
    }

    if (isPlaying_)
    {
        videoInst_->setVolume(baseViewInfo.Volume);

        if (!currentPage_->isMenuScrolling())
        {
            videoInst_->volumeUpdate();

            videoInst_->loopHandler();
        }

        if (baseViewInfo.ImageHeight == 0 && baseViewInfo.ImageWidth == 0)
        {
            baseViewInfo.ImageHeight = static_cast<float>(videoInst_->getHeight());
            baseViewInfo.ImageWidth = static_cast<float>(videoInst_->getWidth());
        }

        bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0;

        if (isCurrentlyVisible)
        {
            hasBeenOnScreen_ = true;
        }

        if (baseViewInfo.PauseOnScroll)
        {
            if (!isCurrentlyVisible && !videoInst_->isPaused() && !currentPage_->isMenuFastScrolling())
            {
                videoInst_->pause();
                if (Logger::isLevelEnabled("DEBUG"))
                    LOG_DEBUG("VideoComponent", "Paused " + Utils::getFileName(videoFile_));
            }
            else if (isCurrentlyVisible && videoInst_->isPaused())
            {
                videoInst_->pause();
                if (Logger::isLevelEnabled("DEBUG"))
                    LOG_DEBUG("VideoComponent", "Resumed " + Utils::getFileName(videoFile_));
            }
        }

        if (baseViewInfo.Restart && hasBeenOnScreen_)
        {
            if (videoInst_->isPaused())
                videoInst_->pause();
            videoInst_->restart();
            baseViewInfo.Restart = false;
            if (Logger::isLevelEnabled("DEBUG"))
                LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
        }
    }

    return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory()
{
    if (videoInst_)
    {
        return;
    }
    else
    {
        Component::allocateGraphicsMemory();
        if (!videoInst_ && !videoFile_.empty())
        {
            videoInst_ = VideoFactory::createVideo(monitor_, numLoops_);
            if (!videoInst_)
            {
                LOG_ERROR("VideoComponent", "Failed to create video instance");
                return;
            }
            isPlaying_ = videoInst_->play(videoFile_);
        }
    }
}

void VideoComponent::freeGraphicsMemory()
{
    if (videoInst_)
    {
        Component::freeGraphicsMemory();
        delete videoInst_;
        isPlaying_ = false;
        if (Logger::isLevelEnabled("DEBUG"))
            LOG_DEBUG("VideoComponent", "Deleted " + Utils::getFileName(videoFile_));
        videoInst_ = nullptr;
    }
}

void VideoComponent::draw()
{
    if (videoInst_)
    {
        // Create the texture if it is not initialized and width/height are known
        if (!textureInitialized_ && videoInst_->getWidth() > 0 && videoInst_->getHeight() > 0)
        {
            videoInst_->createSdlTexture();
            textureInitialized_ = true; // Set the flag to true after creating the texture
        }

        if (textureInitialized_ && baseViewInfo.Alpha > 0.0f && videoInst_->isPlaying())
        {

            if(!videoInst_->isPaused())
                videoInst_->updateTexture();
            

            if (SDL_Texture *texture = videoInst_->getTexture())
            {
                SDL_Rect rect = {static_cast<int>(baseViewInfo.XRelativeToOrigin()),
                                 static_cast<int>(baseViewInfo.YRelativeToOrigin()),
                                 static_cast<int>(baseViewInfo.ScaledWidth()),
                                 static_cast<int>(baseViewInfo.ScaledHeight())};

                LOG_DEBUG("VideoComponent", "Drawing texture...");
                SDL::renderCopy(texture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
                                page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
                                page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
            }
            else
            {
                LOG_ERROR("VideoComponent", "Texture is null. Cannot render.");
            }
        }
    }
}

bool VideoComponent::isPlaying()
{
    return videoInst_->isPlaying();
}

std::string_view VideoComponent::filePath()
{
    return videoFile_;
}

void VideoComponent::skipForward()
{
    if (videoInst_)
        videoInst_->skipForward();
}

void VideoComponent::skipBackward()
{
    if (videoInst_)
        videoInst_->skipBackward();
}

void VideoComponent::skipForwardp()
{
    if (videoInst_)
        videoInst_->skipForwardp();
}

void VideoComponent::skipBackwardp()
{
    if (videoInst_)
        videoInst_->skipBackwardp();
}

void VideoComponent::pause()
{
    if (videoInst_)
        videoInst_->pause();
}

void VideoComponent::restart()
{
    if (videoInst_)
        videoInst_->restart();
}

unsigned long long VideoComponent::getCurrent()
{
    if (videoInst_)
        return videoInst_->getCurrent();
    else
        return 0;
}

unsigned long long VideoComponent::getDuration()
{
    if (videoInst_)
        return videoInst_->getDuration();
    else
        return 0;
}

bool VideoComponent::isPaused()
{
    if (videoInst_)
        return videoInst_->isPaused();
    else
        return false;
}
