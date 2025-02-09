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
#include <string_view>
#include <type_traits>
#include <utility>
#include <memory>

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


VideoComponent::VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listId)
	: Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p)
{
}

VideoComponent::~VideoComponent()
{
	LOG_DEBUG("VideoComponent", "Destroying VideoComponent for file: " + videoFile_);
	VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt)
{
	if (!instanceReady_ || !currentPage_)
	{
		return Component::update(dt);
	}

	if ((currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0)) {
		if (videoInst_->isPaused()) {
			videoInst_->pause();
		}
		return Component::update(dt);
	}

	videoInst_->messageHandler();

	// Check for errors first
	if (videoInst_->hasError()) {
		LOG_DEBUG("VideoComponent", "Detected error in video instance for " +
			Utils::getFileName(videoFile_) + ", destroying and creating new instance");

		// Stop the errored instance
		instanceReady_ = false;
		videoInst_.reset();  // Smart pointer cleanup

		// Get new instance
		videoInst_ = VideoFactory::createVideo(monitor_, numLoops_, softOverlay_, listId_);
		if (videoInst_) {
			instanceReady_ = videoInst_->play(videoFile_);
			if (instanceReady_) {
				LOG_DEBUG("VideoComponent", "Successfully created new instance for " +
					Utils::getFileName(videoFile_));
			}
			else {
				LOG_ERROR("VideoComponent", "Failed to start playback with new instance: " +
					Utils::getFileName(videoFile_));
			}
		}

		return Component::update(dt);
	}
	videoInst_->setVolume(baseViewInfo.Volume);

	if (!currentPage_->isMenuScrolling())
	{
		videoInst_->volumeUpdate();
	}

	float videoHeight = static_cast<float>(videoInst_->getHeight());
	float videoWidth = static_cast<float>(videoInst_->getWidth());

	if (baseViewInfo.ImageHeight != videoHeight || baseViewInfo.ImageWidth != videoWidth ||
		baseViewInfo.ImageHeight == 0 || baseViewInfo.ImageWidth == 0) {
		baseViewInfo.ImageHeight = videoHeight;
		baseViewInfo.ImageWidth = videoWidth;
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
			pause();
			LOG_DEBUG("VideoComponent", "Paused " + Utils::getFileName(videoFile_));
		}
		else if (isCurrentlyVisible && videoInst_->isPaused())
		{
			pause();
			LOG_DEBUG("VideoComponent", "Resumed " + Utils::getFileName(videoFile_));
		}
	}

	if (baseViewInfo.Restart && hasBeenOnScreen_) {
		if (isPaused())
			pause();

		// Wait until the current frame is processed before restarting (if needed)
		if (videoInst_->getCurrent() > 1000000) {
			restart();
			baseViewInfo.Restart = false;
			LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
		}
	}


	return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	if (!instanceReady_) {
		if (!videoInst_ && videoFile_ != "") {
			videoInst_ = VideoFactory::createVideo(monitor_, numLoops_, softOverlay_, listId_);
			if (videoInst_) {
				instanceReady_ = videoInst_->play(videoFile_);
			}
		}
	}
}

void VideoComponent::freeGraphicsMemory()
{
	Component::freeGraphicsMemory();
	if (videoInst_) {
		instanceReady_ = false;

		if (listId_ != -1) {  // Simplified check now that markedForDeletion_ is gone
			if (auto* gstreamerVideo = dynamic_cast<GStreamerVideo*>(videoInst_.get())) {
				LOG_DEBUG("VideoComponent", "Releasing video to pool: " + videoFile_);
				VideoPool::releaseVideo(
					std::unique_ptr<GStreamerVideo>(gstreamerVideo),
					monitor_,
					listId_
				);
				videoInst_.release();  // Release ownership without deletion
				return;
			}
		}

		LOG_DEBUG("VideoComponent", "Stopping and resetting video: " + videoFile_);
		videoInst_->stop();
		videoInst_.reset();  // Clean deletion for non-pooled instances
	}
}

void VideoComponent::draw() {
	if (!videoInst_ || !instanceReady_) return;

	videoInst_->draw();

	if (SDL_Texture* texture = videoInst_->getTexture()) {
		SDL_FRect rect = {
			baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
			baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };

		SDL::renderCopyF(texture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
			page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
			page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
	}
}

std::string_view VideoComponent::filePath() const  // Add const since this doesn't modify state
{
	return videoFile_;  // This is fine as is since string_view is safe with empty strings
}

void VideoComponent::skipForward()
{
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipForward();
}

void VideoComponent::skipBackward()
{
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipBackward();
}

void VideoComponent::skipForwardp()
{
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipForwardp();
}

void VideoComponent::skipBackwardp()
{
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipBackwardp();
}

void VideoComponent::pause()
{
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->pause();
}

void VideoComponent::restart()
{
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->restart();
}

unsigned long long VideoComponent::getCurrent()
{
	if (!videoInst_ || !instanceReady_) {
		return 0;
	}
	return videoInst_->getCurrent();
}

unsigned long long VideoComponent::getDuration()
{
	if (!videoInst_ || !instanceReady_) {
		return 0;
	}
	return videoInst_->getDuration();
}

bool VideoComponent::isPaused()
{
	if (!videoInst_ || !instanceReady_) {
		return false;
	}
	return videoInst_->isPaused();
}

bool VideoComponent::isPlaying()
{
	if (!videoInst_ || !instanceReady_) {
		return false;
	}
	return videoInst_->isPlaying();
}