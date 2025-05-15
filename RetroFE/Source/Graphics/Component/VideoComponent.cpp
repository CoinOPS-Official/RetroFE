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
	if (!instanceReady_ || !currentPage_)
		return Component::update(dt);

	if (videoInst_ && videoInst_->hasError()) { // New check
		LOG_WARNING("VideoComponent", "Update: GStreamerVideo instance for " + videoFile_ +
			" has an error. Halting further video operations for this component.");
		instanceReady_ = false; // Stop trying to interact with a faulty instance
		// The pool will handle the faulty instance upon its release.
		return Component::update(dt);
	}

	if ((currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0)) {
		if (videoInst_->isPaused())
			videoInst_->pause();  // Force pause during game launch
		return Component::update(dt);
	}

	bool isVideoPlaying = videoInst_->isPlaying();
	bool isPaused = videoInst_->isPaused();
	bool isFastScrolling = currentPage_->isMenuFastScrolling();

	if (isVideoPlaying) {
		videoInst_->setVolume(baseViewInfo.Volume);

		if (!currentPage_->isMenuScrolling())
			videoInst_->volumeUpdate();

		// One-time dimension fetch
		if (!dimensionsUpdated_) {
			auto videoHeight = static_cast<float>(videoInst_->getHeight());
			auto videoWidth = static_cast<float>(videoInst_->getWidth());
			if (videoHeight > 0.0f && videoWidth > 0.0f) {
				baseViewInfo.ImageHeight = videoHeight;
				baseViewInfo.ImageWidth = videoWidth;
				dimensionsUpdated_ = true;
				LOG_DEBUG("VideoComponent", "Updated video dimensions: " +
					std::to_string(static_cast<int>(videoWidth)) + "x" + std::to_string(static_cast<int>(videoHeight)) +
					" for " + videoFile_);
			}
		}

		bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0f;
		if (isCurrentlyVisible)
			hasBeenOnScreen_ = true;

		// Only toggle playback state when visibility *changes*
		if (baseViewInfo.PauseOnScroll) {
			if (isCurrentlyVisible != wasVisible_) {
				if (isCurrentlyVisible) {
					videoInst_->resume();
					LOG_DEBUG("VideoComponent", "Resumed " + videoFile_);
				}
				else {
					videoInst_->pause();
					LOG_DEBUG("VideoComponent", "Paused " + videoFile_);
				}
				wasVisible_ = isCurrentlyVisible;
			}
		}

		// Restart support
		if (baseViewInfo.Restart && hasBeenOnScreen_) {
			if (videoInst_->getTargetState() == IVideo::VideoState::Playing && videoInst_->isPaused())
				videoInst_->resume();

			GstClockTime currentTime = videoInst_->getCurrent();
			if (currentTime > 1000000) {
				videoInst_->restart();
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
				if (!instanceReady_) {
					LOG_WARNING("VideoComponent", "play() returned false for: " + videoFile_);
					if (videoInst_->hasError()) {
						LOG_ERROR("VideoComponent", "GStreamerVideo instance for " + videoFile_ +
							" is marked with hasError_ after play() attempt. Video will not display.");
						// The videoInst_ is now "tainted".
						// VideoPool will handle its disposal when freeGraphicsMemory() is called.
					}
					else {
						LOG_WARNING("VideoComponent", "play() returned false for " + videoFile_ +
							" but GStreamerVideo instance not marked with error. (e.g. file not found by GStreamer, URI error before pipeline interaction)");
						// This is a non-fatal error, but the video will not display.	
					}
					// instanceReady_ is false, so draw() should not attempt to use it.
				}
				else {
					LOG_DEBUG("VideoComponent", "play() succeeded for: " + videoFile_);
					// Potentially set volume, loops here if needed, though GStreamerVideo::play handles initial mute.
				}
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
		//videoInst_->stop();
		videoInst_.reset();  // Clean deletion for non-pooled instances
	}
}

void VideoComponent::draw() {
	if (!videoInst_ || !instanceReady_ || !videoInst_->isPlaying()) return;

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

void VideoComponent::resume() {
	if (!videoInst_ || !instanceReady_) {
		return;
	}
	videoInst_->resume();
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