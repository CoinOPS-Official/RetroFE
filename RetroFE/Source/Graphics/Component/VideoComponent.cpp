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

#include "../../Video/GStreamerVideo.h"

#include "../../Graphics/ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../Video/IVideo.h"
#include "../../Video/VideoFactory.h"
#include "../Page.h"
#ifdef __APPLE__
#include "SDL2/SDL_rect.h"
#include "SDL2/SDL_render.h"
#else
#include "SDL_rect.h"
#include "SDL_render.h"
#endif
#include <gst/video/video.h>
#include "../../Video/VideoPool.h"


VideoComponent::VideoComponent(Page &p, const std::string &videoFile, int monitor, int numLoops, bool softOverlay, int listId)
	: Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p)
{
}

VideoComponent::~VideoComponent()
{
    VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt)
{
    if (!instanceReady_)
    {
        return Component::update(dt);
    }

    else
    {
        videoInst_->setVolume(baseViewInfo.Volume);
        // videoInst_->update(dt);
        if (!currentPage_->isMenuScrolling())
        {
            videoInst_->volumeUpdate();
        }

		videoInst_->loopHandler();

        if (baseViewInfo.ImageHeight == 0 && baseViewInfo.ImageWidth == 0)
        {
            baseViewInfo.ImageHeight = static_cast<float>(videoInst_->getHeight());
            baseViewInfo.ImageWidth = static_cast<float>(videoInst_->getWidth());
        }

        bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0f;

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

        if (baseViewInfo.Restart && hasBeenOnScreen_) {
            if (videoInst_->isPaused())
                videoInst_->pause();

            // Wait until the current frame is processed before restarting (if needed)
            if (videoInst_->getCurrent() > 1000000) {
                videoInst_->restart();
                baseViewInfo.Restart = false;
                LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
            }
        }
    }

    return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory()
{
    Component::allocateGraphicsMemory();

    if (!instanceReady_) {  // Was isPlaying_
        if (!videoInst_ && videoFile_ != "") {
            videoInst_ = VideoFactory::createVideo(monitor_, numLoops_, softOverlay_, listId_);
            instanceReady_ = videoInst_->play(videoFile_);  // Was isPlaying_
        }
    }
}

void VideoComponent::freeGraphicsMemory()
{
    Component::freeGraphicsMemory();

    if (videoInst_) {
        instanceReady_ = false;  // Set to false FIRST to prevent updates during release
        GStreamerVideo* gstreamerVideo = static_cast<GStreamerVideo*>(videoInst_);
        VideoPool::releaseVideo(gstreamerVideo, monitor_, listId_);
        videoInst_ = nullptr;
    }
}

void VideoComponent::draw() {
    if (videoInst_ && instanceReady_) {
        if (videoInst_->isPlaying()) {
            videoInst_->draw();
        }
        if (SDL_Texture* texture = videoInst_->getTexture()) {
            SDL_FRect rect = {
                baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
                baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };

            SDL::renderCopyF(texture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
                page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
                page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
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
