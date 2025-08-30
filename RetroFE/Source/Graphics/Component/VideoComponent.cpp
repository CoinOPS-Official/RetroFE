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
    if (!videoInst_ || !currentPage_ || !instanceReady_)
        return Component::update(dt);

    if (!videoInst_->isPipelineReady())
        return Component::update(dt);

    if (videoInst_->hasError()) {
        LOG_WARNING("VideoComponent", "Update: GStreamerVideo instance for " + videoFile_ +
            " has an error. Halting further video operations for this component.");
        return Component::update(dt);
    }

    // Dimensions once available
    if (!dimensionsUpdated_) {
        const int w = videoInst_->getWidth(), h = videoInst_->getHeight();
        if (w <= 0 || h <= 0) return Component::update(dt);
        baseViewInfo.ImageWidth = static_cast<float>(w);
        baseViewInfo.ImageHeight = static_cast<float>(h);
        dimensionsUpdated_ = true;
        LOG_DEBUG("VideoComponent", "Video dimensions ready: " +
            std::to_string(w) + "x" + std::to_string(h) + " for " + videoFile_);
    }

    // Volume
    videoInst_->setVolume(baseViewInfo.Volume);
    if (!currentPage_->isMenuScrolling())
        videoInst_->volumeUpdate();

    // Snapshot state (needed before fast-scroll branch)
    const auto actual = videoInst_->getActualState();
    const auto target = videoInst_->getTargetState();
    const bool inFlight = (target != actual);

    // Hard rule: if game launched on primary monitor -> pause and return
    if (currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0) {
        if (actual != IVideo::VideoState::Paused &&
            target != IVideo::VideoState::Paused) {
            videoInst_->pause();
        }
        return Component::update(dt);
    }

    if (currentPage_->isMenuFastScrolling()) {
        if (!inFlight &&
            actual != IVideo::VideoState::Playing &&
            target != IVideo::VideoState::Playing) {
            videoInst_->resume();
            LOG_DEBUG("VideoComponent", "Force-resume during fast scroll: " + videoFile_);
        }
        // No restart while flapping
        if (baseViewInfo.Restart) {
            baseViewInfo.Restart = false;
        }
        pendingRestart_ = false;   // also drop any deferred restart
        // Early return to avoid any other state changes in this frame
        return Component::update(dt);
    }

    // Visibility
    const bool visibleNow = (baseViewInfo.Alpha > 0.0f);
    if (visibleNow) hasBeenOnScreen_ = true;

    // ---------------- Desired state ----------------
    IVideo::VideoState desired = IVideo::VideoState::None;

    // Always ensure play when visible (original auto-play)
    if (visibleNow) {
        desired = IVideo::VideoState::Playing;
    }

    // Pause-on-scroll: pause only on the HIDE edge
    if (baseViewInfo.PauseOnScroll && !visibleNow && wasVisible_) {
        desired = IVideo::VideoState::Paused;
    }

    // Apply at most one transition, only if not in-flight
    if (!inFlight && desired != IVideo::VideoState::None) {
        if (desired == IVideo::VideoState::Playing && actual != IVideo::VideoState::Playing) {
            videoInst_->resume();
            LOG_DEBUG("VideoComponent", "Resume -> " + videoFile_);
        }
        else if (desired == IVideo::VideoState::Paused && actual != IVideo::VideoState::Paused) {
            videoInst_->pause();
            LOG_DEBUG("VideoComponent", "Pause -> " + videoFile_);
        }
    }

    // Restart handling (two-phase, no timers)
    if (baseViewInfo.Restart && hasBeenOnScreen_) {
        if (!inFlight) {
            if (actual == IVideo::VideoState::Paused) {
                videoInst_->resume();      // get to PLAYING first
                pendingRestart_ = true;    // perform seek once PLAYING
                LOG_DEBUG("VideoComponent", "Deferred restart: resuming first for " + videoFile_);
            }
            else if (actual == IVideo::VideoState::Playing) {
                if (videoInst_->getCurrent() > GST_SECOND) {
                    videoInst_->restart();
                    LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
                }
                baseViewInfo.Restart = false;
                pendingRestart_ = false;
            }
            else {
                pendingRestart_ = true; // READY/transitioning: defer
            }
        }
        else {
            pendingRestart_ = true;     // defer while transitioning
        }
    }

    // Complete deferred restart once PLAYING & not in-flight
    if (pendingRestart_ && !inFlight && videoInst_->getActualState() == IVideo::VideoState::Playing) {
        if (videoInst_->getCurrent() > GST_SECOND) {
            videoInst_->restart();
            LOG_DEBUG("VideoComponent", "Post-transition seek to start: " + Utils::getFileName(videoFile_));
        }
        baseViewInfo.Restart = false;
        pendingRestart_ = false;
    }

    wasVisible_ = visibleNow;
    return Component::update(dt);
}


void VideoComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	if (videoInst_) {
		return;
	}
	if (!videoFile_.empty()) {
		videoInst_ = VideoFactory::createVideo(
			monitor_, numLoops_, softOverlay_, listId_,
			hasPerspective_ ? perspectiveCorners_ : nullptr
		);

		if (!videoInst_) {
			LOG_ERROR("VideoComponent", "Failed to create a video instance from the factory.");
		}
		else {
			LOG_DEBUG("VideoComponent", "Issuing play command for: " + videoFile_);
			instanceReady_ = videoInst_->play(videoFile_);
		}
	}
}

std::unique_ptr<IVideo> VideoComponent::extractVideo() {
	instanceReady_ = false;
	return std::move(videoInst_);
}

void VideoComponent::freeGraphicsMemory() {
	Component::freeGraphicsMemory();

	if (!videoInst_) return;  // Already extracted

	if (listId_ != -1) {
		LOG_DEBUG("VideoComponent", "Releasing video to pool: " + videoFile_);
		auto video = std::move(videoInst_);
		VideoPool::releaseVideo(std::move(video), monitor_, listId_);
		return;
	}

	LOG_DEBUG("VideoComponent", "Stopping and resetting video: " + videoFile_);
	videoInst_.reset();
}


void VideoComponent::draw() {
	if (!videoInst_ || !currentPage_ || !instanceReady_) {
		return;
	}

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

std::string_view VideoComponent::filePath() {
	return videoFile_;
}

void VideoComponent::skipForward() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipForward();
}

void VideoComponent::skipBackward() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipBackward();
}

void VideoComponent::skipForwardp() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipForwardp();
}

void VideoComponent::skipBackwardp() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipBackwardp();
}

void VideoComponent::pause() {
	if (!videoInst_) {
		return;
	}
	videoInst_->pause();
}

void VideoComponent::resume() {
	if (!videoInst_) {
		return;
	}
	videoInst_->resume();
}


void VideoComponent::restart() {
	if (!videoInst_) {
		return;
	}
	videoInst_->restart();
}

unsigned long long VideoComponent::getCurrent() {
	if (!videoInst_) {
		return 0;
	}
	return videoInst_->getCurrent();
}

unsigned long long VideoComponent::getDuration() {
	if (!videoInst_) {
		return 0;
	}
	return videoInst_->getDuration();
}

bool VideoComponent::isPaused() {
	if (!videoInst_) {
		return false;
	}
	return videoInst_->isPaused();
}

bool VideoComponent::isPlaying() {
	if (!videoInst_) {
		return false;
	}
	return videoInst_->isPlaying();
}