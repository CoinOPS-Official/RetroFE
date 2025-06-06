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


VideoComponent::VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listId, const int* perspectiveCorners)
	: Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p)
{
	if (perspectiveCorners) {
		std::copy(perspectiveCorners, perspectiveCorners + 8, perspectiveCorners_);
		hasPerspective_ = true;
	}
}

VideoComponent::~VideoComponent()
{
	LOG_DEBUG("VideoComponent", "Destroying VideoComponent for file: " + videoFile_);
	VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt) {
	if (!instanceReady_ || !currentPage_) {
		return Component::update(dt);
	}

	if ((currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0)) {
		if (videoInst_->isPaused()) {
			videoInst_->pause();  // Ensure paused during launch
		}
		return Component::update(dt);
	}

	videoInst_->messageHandler(dt);

	// Check for errors first
	bool hasError = videoInst_->hasError();
	if (hasError) {
		LOG_DEBUG("VideoComponent", "Detected error in video instance for " +
			Utils::getFileName(videoFile_) + ", destroying and creating new instance");
		instanceReady_ = false;
		videoInst_.reset();
		videoInst_ = VideoFactory::createVideo(monitor_, numLoops_, softOverlay_, listId_, perspectiveCorners_);
		if (videoInst_) {
			instanceReady_ = videoInst_->play(videoFile_);
			dimensionsUpdated_ = false; // Reset flag for new instance
			if (!instanceReady_) {
				LOG_ERROR("VideoComponent", "Failed to start playback with new instance: " +
					Utils::getFileName(videoFile_));
			}
		}
		return Component::update(dt);
	}

	// Cache the playing state once
	bool isVideoPlaying = videoInst_->isPlaying();
	bool isPaused = videoInst_->isPaused();

	// Only proceed with state changes if video is fully playing
	if (isVideoPlaying) {
		videoInst_->setVolume(baseViewInfo.Volume);

		if (!currentPage_->isMenuScrolling()) {
			videoInst_->volumeUpdate();
		}

		// Only update dimensions once after playback starts
		if (!dimensionsUpdated_) {
			auto videoHeight = static_cast<float>(videoInst_->getHeight());
			auto videoWidth = static_cast<float>(videoInst_->getWidth());

			// Only update if we have valid dimensions
			if (videoHeight > 0.0f && videoWidth > 0.0f) {
				baseViewInfo.ImageHeight = videoHeight;
				baseViewInfo.ImageWidth = videoWidth;
				dimensionsUpdated_ = true; // Mark dimensions as updated

				LOG_DEBUG("VideoComponent", "Updated video dimensions: " +
					std::to_string(videoWidth) + "x" + std::to_string(videoHeight) +
					" for " + Utils::getFileName(videoFile_));
			}
		}

		bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0f;
		if (isCurrentlyVisible) {
			hasBeenOnScreen_ = true;
		}

		if (baseViewInfo.PauseOnScroll) {
			if (!isCurrentlyVisible && !isPaused && !currentPage_->isMenuFastScrolling()) {
				pause();
				LOG_DEBUG("VideoComponent", "Paused " + Utils::getFileName(videoFile_));
			}
			else if (isCurrentlyVisible && isPaused) {
				pause();
				LOG_DEBUG("VideoComponent", "Resumed " + Utils::getFileName(videoFile_));
			}
		}

		if (baseViewInfo.Restart && hasBeenOnScreen_) {
			if (isPaused) {
				pause();  // Resume if paused
			}

			GstClockTime currentTime = videoInst_->getCurrent();
			if (currentTime > 1000000) {
				restart();
				baseViewInfo.Restart = false;
				LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
			}
		}
	}

	return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	if (!instanceReady_) {
		if (!videoInst_ && videoFile_ != "") {
			videoInst_ = VideoFactory::createVideo(monitor_, numLoops_, softOverlay_, listId_,
				hasPerspective_ ? perspectiveCorners_ : nullptr);
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