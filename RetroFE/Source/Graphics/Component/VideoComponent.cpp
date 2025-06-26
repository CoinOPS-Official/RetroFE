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
#include "../../Utility/ThreadPool.h"
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
	: Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p) {
	if (perspectiveCorners) {
		std::copy(perspectiveCorners, perspectiveCorners + 8, perspectiveCorners_);
		hasPerspective_ = true;
	}
}

VideoComponent::~VideoComponent() {
	LOG_DEBUG("VideoComponent", "Destroying VideoComponent for file: " + videoFile_);
	VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt) {
	if (!instanceReady_ || !videoInst_ || !currentPage_)
		return Component::update(dt);

	if (videoInst_->hasError()) { // New check
		LOG_WARNING("VideoComponent", "Update: GStreamerVideo instance for " + videoFile_ +
			" has an error. Halting further video operations for this component.");
		instanceReady_ = false; // Stop trying to interact with a faulty instance
		// The pool will handle the faulty instance upon its release.
		return Component::update(dt);
	}

	if (!dimensionsUpdated_) {
		if (videoInst_->getWidth() >= 0 && videoInst_->getHeight() >= 0) {
			baseViewInfo.ImageWidth = static_cast<float>(videoInst_->getWidth());
			baseViewInfo.ImageHeight = static_cast<float>(videoInst_->getHeight());
			dimensionsUpdated_ = true;
			LOG_DEBUG("VideoComponent", "Video dimensions ready: " +
				std::to_string(videoInst_->getWidth()) + "x" +
				std::to_string(videoInst_->getHeight()) + " for " + videoFile_);
		}
		else {
			return Component::update(dt);
		}
	}

	if ((currentPage_->getIsLaunched() && monitor_ == 0)) {
		if (videoInst_->getTargetState() != IVideo::VideoState::Paused &&
			videoInst_->getActualState() != IVideo::VideoState::Paused) {
			videoInst_->pause();
		}
		return Component::update(dt);
	}

	if (currentPage_->isMenuFastScrolling()) {
		// Always ensure video is playing during fast scroll
		if (videoInst_->getTargetState() != IVideo::VideoState::Playing &&
			videoInst_->getActualState() != IVideo::VideoState::Playing) {
			videoInst_->resume();
			LOG_DEBUG("VideoComponent", "Force-resume during fast scroll: " + videoFile_);
		}
		// No restart logic here, as we want the video to continue playing
		if (baseViewInfo.Restart) {
			baseViewInfo.Restart = false;
			LOG_DEBUG("VideoComponent", "Restarted (fast scroll) " + Utils::getFileName(videoFile_));
		}
		return Component::update(dt);
	}

	videoInst_->setVolume(baseViewInfo.Volume);

	if (!currentPage_->isMenuScrolling())
		videoInst_->volumeUpdate();

	bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0f;
	if (isCurrentlyVisible)
		hasBeenOnScreen_ = true;

	if (isCurrentlyVisible) {
		if (videoInst_->getTargetState() != IVideo::VideoState::Playing &&
			videoInst_->getActualState() != IVideo::VideoState::Playing) {
			videoInst_->resume();
			LOG_DEBUG("VideoComponent", "Auto-played (PauseOnScroll false) " + videoFile_);
		}
	}

	// --- Only toggle playback state when visibility changes (pause on hide, as before) ---
	if (baseViewInfo.PauseOnScroll && (isCurrentlyVisible != wasVisible_)) {
		if (!isCurrentlyVisible) {
			if (videoInst_->getTargetState() != IVideo::VideoState::Paused &&
				videoInst_->getActualState() != IVideo::VideoState::Paused) {
				videoInst_->pause();
				LOG_DEBUG("VideoComponent", "Paused " + videoFile_);
			}
		}
	}
	wasVisible_ = isCurrentlyVisible; // Always update!

	// Restart support
	if (baseViewInfo.Restart && hasBeenOnScreen_) {
		if (videoInst_->getTargetState() == IVideo::VideoState::Paused &&
			videoInst_->getActualState() == IVideo::VideoState::Paused) {
			videoInst_->resume();
		}

		GstClockTime currentTime = videoInst_->getCurrent();
		if (currentTime > GST_SECOND) {
			videoInst_->restart();
			baseViewInfo.Restart = false;
			LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
		}
	}


	return Component::update(dt);
}


void VideoComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();

	if (instanceReady_) return; // Already ready

	if (!videoInst_ && !videoFile_.empty()) {
		videoInst_ = VideoFactory::createVideo(
			monitor_, numLoops_, softOverlay_, listId_,
			hasPerspective_ ? perspectiveCorners_ : nullptr
		);
		if (videoInst_) {
			LOG_DEBUG("VideoComponent", "ThreadPool: play() starting for: " + videoFile_);
			bool result = videoInst_->play(videoFile_);
			instanceReady_ = result;
			if (result) {
				LOG_DEBUG("VideoComponent", "ThreadPool: play() finished SUCCESS for: " + videoFile_);
			}
			else {
				LOG_WARNING("VideoComponent", "ThreadPool: play() finished FAIL for: " + videoFile_);
				if (videoInst_->hasError()) {
					LOG_ERROR("VideoComponent", "GStreamerVideo instance for " + videoFile_ +
						" is marked with hasError_ after play() attempt. Video will not display.");
					// The videoInst_ is now "tainted". Pool will handle on freeGraphicsMemory.
				}
				else {
					LOG_WARNING("VideoComponent", "play() returned false for " + videoFile_ +
						" but not marked as error (could be file not found or URI error).");
				}
			}
		}
	}
}


void VideoComponent::freeGraphicsMemory() {
	Component::freeGraphicsMemory();
	if (videoInst_) {
		instanceReady_ = false;

		// Return to pool if pooled (listId_ != -1), else delete
		if (listId_ != -1) {
			LOG_DEBUG("VideoComponent", "Releasing video to pool: " + videoFile_);
			// Pass ownership as std::unique_ptr<IVideo>
			VideoPool::releaseVideo(std::move(videoInst_), monitor_, listId_);
			// videoInst_ now nullptr
			return;
		}
		LOG_DEBUG("VideoComponent", "Stopping and resetting video: " + videoFile_);
		videoInst_.reset();
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

std::string_view VideoComponent::filePath() const {
	return videoFile_;
}

void VideoComponent::skipForward() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipForward();
}

void VideoComponent::skipBackward() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipBackward();
}

void VideoComponent::skipForwardp() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipForwardp();
}

void VideoComponent::skipBackwardp() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->skipBackwardp();
}

void VideoComponent::pause() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->pause();
}

void VideoComponent::resume() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->resume();
}


void VideoComponent::restart() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->restart();
}

unsigned long long VideoComponent::getCurrent() {
	if (!videoInst_ || !instanceReady_) {
		return 0;
	}
	return videoInst_->getCurrent();
}

unsigned long long VideoComponent::getDuration() {
	if (!videoInst_ || !instanceReady_) {
		return 0;
	}
	return videoInst_->getDuration();
}

bool VideoComponent::isPaused() {
	if (!videoInst_ || !instanceReady_) {
		return false;
	}
	return videoInst_->isPaused();
}

bool VideoComponent::isPlaying() {
	if (!videoInst_ || !instanceReady_) {
		return false;
	}
	return videoInst_->isPlaying();
}