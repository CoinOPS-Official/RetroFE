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
#include <utility>
#include <memory>
#include <algorithm>

#include "../../Graphics/ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Video/IVideo.h"
#include "../../Video/VideoFactory.h"
#include "../../Video/VideoPool.h"
#include "../Page.h"

#ifdef __APPLE__
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#else
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#endif

VideoComponent::VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listId, const int* perspectiveCorners)
    : Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p) {
    isHighPriority_ = (listId_ == -1);
    if (perspectiveCorners) {
        std::copy(perspectiveCorners, perspectiveCorners + 8, perspectiveCorners_);
        hasPerspective_ = true;
    }
}

VideoComponent::~VideoComponent() {
    LOG_DEBUG("VideoComponent", "Destroying VideoComponent for file: " + videoFile_);
    VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::recycleAsVideo(const std::string& path, const std::string&) {
    if (path.empty()) return false;

    if (videoFile_ == path && videoInst_ && !videoInst_->hasError()) {
        return true;
    }

    this->Component::freeGraphicsMemory();
    videoFile_ = path;

    // Reset state flags
    instanceReady_ = false;
    dimensionsUpdated_ = false;
	baseViewInfo.ImageWidth = 0;
	baseViewInfo.ImageHeight = 0;
    hasBeenOnScreen_ = false;
    wasVisible_ = false;
    wasPlayingBeforeFastScroll_ = false;
    isHighPriority_ = (listId_ == -1);
    desiredState_ = PlaybackTarget::Paused;
    pendingCommand_ = PlaybackCommand::None;
    pendingVideoRetry_ = false;
    retryAttempts_ = 0;
    nextRetryTime_ = 0;
    lastVolume_ = -1.0f;

    // --- THE FIX ---
    if (videoInst_) {
        // Do not manually unload. Route it through the pool's safety queues.
        if (listId_ != -1) {
            auto video = std::move(videoInst_);
            VideoPool::releaseVideo(std::move(video), monitor_, listId_);
        }
        else {
            // Standalone videos bypass the pool, so we safely destroy the pipeline
            videoInst_.reset();
        }
    }

    // Acquire a fresh, fully-drained pipeline
    allocateGraphicsMemory();

    return true;
}

bool VideoComponent::checkVisibility() const {
    float x = baseViewInfo.XRelativeToOrigin();
    float y = baseViewInfo.YRelativeToOrigin();
    float w = baseViewInfo.ScaledWidth();
    float h = baseViewInfo.ScaledHeight();

    float screenW = static_cast<float>(currentPage_->getLayoutWidthByMonitor(baseViewInfo.Monitor));
    float screenH = static_cast<float>(currentPage_->getLayoutHeightByMonitor(baseViewInfo.Monitor));

    bool physicallyOnScreen = (x + w > 0.0f) && (x < screenW) && (y + h > 0.0f) && (y < screenH);
    return (baseViewInfo.Alpha > 0.0f) && physicallyOnScreen;
}

void VideoComponent::computeDesiredIntent(bool visibleNow, const VideoSnapshot& snap) {
    // 1. Edge detection for "Rewind on Hide" (so it starts fresh next time user sees it)
    if (!visibleNow && wasVisible_ && hasBeenOnScreen_) {
        pendingCommand_ = PlaybackCommand::Restart;
        wasVisible_ = visibleNow;
        return;
    }
    wasVisible_ = visibleNow;

    // 2. Standalone Logic
    if (listId_ == -1) {
        desiredState_ = visibleNow ? PlaybackTarget::Playing : PlaybackTarget::Paused;
        return;
    }

    // 3. Launch Lock
    if (currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0) {
        desiredState_ = PlaybackTarget::Paused;
        return;
    }

    // 4. Fast Scroll Snapshot
    if (!currentPage_->isMenuFastScrolling()) {
        wasPlayingBeforeFastScroll_ = (snap.targetState == IVideo::VideoState::Playing);
    }

    // 5. Main List Navigation Logic
    if (visibleNow && !snap.hasFinishedLoops) {
        if (currentPage_->isMenuFastScrolling()) {
            // Keep playing if already active, stay paused if new/dormant
            desiredState_ = wasPlayingBeforeFastScroll_ ? PlaybackTarget::Playing : PlaybackTarget::Paused;
        }
        else {
            desiredState_ = PlaybackTarget::Playing;
        }
    }
    else {
        desiredState_ = PlaybackTarget::Paused;
    }

    // 6. Explicit Restart Request (e.g., from Theme animations)
    if (baseViewInfo.Restart && visibleNow) {
        pendingCommand_ = PlaybackCommand::Restart;
        baseViewInfo.Restart = false; // Consume the trigger
    }
}

void VideoComponent::syncPlaybackIntent(const VideoSnapshot& snap) {
    if (!instanceReady_ || !snap.pipelineReady || snap.hasError) {
        return; // Backend is not ready to receive commands yet
    }

    // Flag when we first successfully play on screen
    if (snap.targetState == IVideo::VideoState::Playing && !hasBeenOnScreen_) {
        hasBeenOnScreen_ = true;
    }

    // Dispatch the intended transient command
    if (pendingCommand_ == PlaybackCommand::Restart) {
        videoInst_->restart();
        pendingCommand_ = PlaybackCommand::None; // Consume it
    }

    // Dispatch the persistent state (backend deduplication protects against spam)
    if (desiredState_ == PlaybackTarget::Playing) {
        videoInst_->resume();
    }
    else if (desiredState_ == PlaybackTarget::Paused) {
        videoInst_->pause();
    }
}

bool VideoComponent::update(float dt) {
    bool visibleNow = checkVisibility();

    // 1. Fast-abort if hidden (Lazy Activation)
    if (!visibleNow) {
        pendingVideoRetry_ = false; // Drop retries if user scrolls past
        if (!videoInst_) return Component::update(dt);
    }

    // 2. Enforce the Retry Backoff Timer!
    // This stops the 60fps log spam if the pool OR the CPU is full.
    if (pendingVideoRetry_) {
        if (SDL_GetTicks() < nextRetryTime_) {
            return Component::update(dt); // Wait patiently
        }
    }

    // 3. Opportunistic acquisition if pool was previously saturated
    if (!videoInst_ && !videoFile_.empty()) {
        allocateGraphicsMemory();
        if (!videoInst_) {
            // Pool is full! Trigger exponential backoff.
            pendingVideoRetry_ = true;
            retryAttempts_++;
            const uint32_t delay = std::min(250u, 16u * (1u << std::min(retryAttempts_, 4u)));
            nextRetryTime_ = SDL_GetTicks() + delay;
            return Component::update(dt);
        }
    }

    if (!videoInst_ || !currentPage_) {
        return Component::update(dt);
    }

    // 4. Retry / Initiate playback orchestrator
    if (!instanceReady_ && !videoInst_->hasError()) {

        instanceReady_ = videoInst_->open(videoFile_);

        if (!instanceReady_) {
            // CPU Preroll limit hit! Trigger exponential backoff.
            pendingVideoRetry_ = true;
            retryAttempts_ = std::max(1u, retryAttempts_ + 1); // Use std::max to ensure we scale correctly
            const uint32_t delay = std::min(250u, 16u * (1u << std::min(retryAttempts_, 4u)));
            nextRetryTime_ = SDL_GetTicks() + delay;
            return Component::update(dt);
        }
        else {
            // Success! Clear the retry state.
            pendingVideoRetry_ = false;
            retryAttempts_ = 0;
        }
    }

    if (instanceReady_) videoInst_->updateFrame();

    // --- ATOMIC SNAPSHOT PULL ---
    const auto snap = videoInst_->getSnapshot();
	currentSnapshot_ = snap; // Store it for draw() and future logic

    // Wait for the pipeline to spin up
    if (!instanceReady_ || !snap.pipelineReady || snap.hasError) {
        return Component::update(dt);
    }

    // 5. Dimension Locking & Audio
    if (!dimensionsUpdated_) {
        if (!snap.hasVideoStream || (videoInst_->getDimensions().w > 0 && videoInst_->getDimensions().h > 0)) {
            if (snap.hasVideoStream) {
                baseViewInfo.ImageWidth = static_cast<float>(videoInst_->getDimensions().w);
                baseViewInfo.ImageHeight = static_cast<float>(videoInst_->getDimensions().h);
            }
            dimensionsUpdated_ = true;
        }
        else {
            return Component::update(dt);
        }
    }

    if (std::abs(baseViewInfo.Volume - lastVolume_) > 1e-4f) {
        lastVolume_ = baseViewInfo.Volume;
        videoInst_->setVolume(baseViewInfo.Volume);
    }

    // 6. --- ORCHESTRATION PIPELINE ---
    computeDesiredIntent(visibleNow, snap);
    syncPlaybackIntent(snap);

    return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory() {
    Component::allocateGraphicsMemory();
    if (videoInst_) return;

    if (!videoFile_.empty()) {
        videoInst_ = VideoFactory::createVideo(
            monitor_, numLoops_, softOverlay_, listId_,
            hasPerspective_ ? perspectiveCorners_ : nullptr
        );
        instanceReady_ = false;
    }
}

std::shared_ptr<IVideo> VideoComponent::extractVideo() {
    instanceReady_ = false;
    return std::move(videoInst_);
}

void VideoComponent::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    if (!videoInst_) return;

    if (listId_ != -1) {
        auto video = std::move(videoInst_);
        VideoPool::releaseVideo(std::move(video), monitor_, listId_);
        return;
    }

    videoInst_.reset();
}

void VideoComponent::draw() {
    if (!videoInst_ || !currentPage_ || !instanceReady_) {
        return;
    }


    SDL_Texture* texture = videoInst_->getTexture();

    if (texture) {
        SDL_FRect rect = {
            baseViewInfo.XRelativeToOrigin(),
            baseViewInfo.YRelativeToOrigin(),
            baseViewInfo.ScaledWidth(),
            baseViewInfo.ScaledHeight()
        };

        const auto dimensions = videoInst_->getDimensions();
        SDL_Rect source{0, 0, dimensions.w, dimensions.h};
        SDL::renderCopyF(
            texture,
            baseViewInfo.Alpha,
            &source,
            &rect,
            baseViewInfo,
            page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
            page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
    }
}
std::string_view VideoComponent::filePath() { return videoFile_; }

void VideoComponent::skipForward() { if (videoInst_) videoInst_->skipForward(); }
void VideoComponent::skipBackward() { if (videoInst_) videoInst_->skipBackward(); }
void VideoComponent::skipForwardp() { if (videoInst_) videoInst_->skipForwardp(); }
void VideoComponent::skipBackwardp() { if (videoInst_) videoInst_->skipBackwardp(); }
void VideoComponent::pause() { if (videoInst_) videoInst_->pause(); }
void VideoComponent::resume() { if (videoInst_) videoInst_->resume(); }
void VideoComponent::restart() { if (videoInst_) videoInst_->restart(); }

unsigned long long VideoComponent::getCurrent() { return videoInst_ ? videoInst_->getCurrent() : 0; }
unsigned long long VideoComponent::getDuration() { return videoInst_ ? videoInst_->getDuration() : 0; }
bool VideoComponent::isPaused() { return videoInst_ ? videoInst_->isPaused() : false; }
bool VideoComponent::isPlaying() { return videoInst_ ? videoInst_->isPlaying() : false; }
bool VideoComponent::hasFinishedLoops() { return videoInst_ ? videoInst_->hasFinishedLoops() : true; }
bool VideoComponent::hasVideoStream() { return videoInst_ ? videoInst_->hasVideoStream() : false; }
