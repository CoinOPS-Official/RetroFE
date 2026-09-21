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
#include "Image.h"
#include <string>
#include <string_view>
#include <utility>
#include <memory>
#include <algorithm>
#include <cmath>
#include <vector>

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

// Components and their startup decisions are owned by the main thread.
static std::vector<VideoComponent*> listVideos;

VideoComponent::VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listId, const int* perspectiveCorners)
    : Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p) {
    isHighPriority_ = (listId_ == -1);
    if (listId_ != -1) listVideos.push_back(this);
    if (perspectiveCorners) {
        std::copy(perspectiveCorners, perspectiveCorners + 8, perspectiveCorners_);
        hasPerspective_ = true;
    }
}

VideoComponent::~VideoComponent() {
    std::erase(listVideos, this);
    LOG_DEBUG("VideoComponent", "Destroying VideoComponent for file: " + videoFile_);
    VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::recycleAsVideo(const std::string& path, const std::string&) {
    if (path.empty()) return false;

    // One-shot request used by same-list jumps. Preserve the currently-owned
    // warm pipeline while changing the component's media target.
    bool preserveInstance = preserveInstanceOnNextRecycle_;
    preserveInstanceOnNextRecycle_ = false;

    if (videoFile_ == path && videoInst_ && !videoInst_->hasError()) {
        return true;
    }

    // An active pipeline may still contain queued samples from the previous
    // URI. Quiesce and drain it before keeping it attached for this retarget.
    // If that cannot be done safely, fall back to the normal release/reacquire
    // path rather than risk displaying stale media.
    if (preserveInstance && videoInst_) {
        auto* gstVideo = videoInst_.get();
        if (!gstVideo || !gstVideo->prepareForRetarget()) {
            preserveInstance = false;
        }
    }

    startupArtwork_.reset();
    this->Component::freeGraphicsMemory();
    videoFile_ = path;

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

    if (videoInst_ && !preserveInstance) {
        if (listId_ != -1) {
            auto video = std::move(videoInst_);
            VideoPool::releaseVideo(std::move(video), monitor_, listId_);
        }
        else {
            videoInst_.reset();
        }
    }

    // ScrollingList calls allocateGraphicsMemory() after rebinding the slot.
    // With preserveInstance=true, videoInst_ remains attached and the next
    // update retargets the same warm video instance to videoFile_.
    return true;
}

void VideoComponent::preserveInstanceOnNextRecycle() {
    preserveInstanceOnNextRecycle_ = true;
}

bool VideoComponent::prepareRetainedVideoForRetarget() {
    if (!videoInst_)
        return false;

    auto* gstVideo = videoInst_.get();
    if (!gstVideo)
        return false;

    return gstVideo->prepareForRetarget();
}

bool VideoComponent::checkVisibility() const {
    if (baseViewInfo.Alpha <= 0.0f) return false;
    float x = baseViewInfo.XRelativeToOrigin();
    float y = baseViewInfo.YRelativeToOrigin();
    float w = baseViewInfo.ScaledWidth();
    float h = baseViewInfo.ScaledHeight();
    // Intrinsic video dimensions may be unknown until preroll. Do not prevent
    // that first decode merely because an auto-sized component has no bounds.
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0.0f || h <= 0.0f) return true;

    float screenW = static_cast<float>(currentPage_->getLayoutWidthByMonitor(baseViewInfo.Monitor));
    float screenH = static_cast<float>(currentPage_->getLayoutHeightByMonitor(baseViewInfo.Monitor));

    bool physicallyOnScreen = (x + w > 0.0f) && (x < screenW) && (y + h > 0.0f) && (y < screenH);
    return (baseViewInfo.Alpha > 0.0f) && physicallyOnScreen;
}

bool VideoComponent::canStartBackgroundVideo() const {
    // The selected/destination slot is presentation-critical. It may still be
    // hidden at the first update of a tween (alpha 0 / just off-screen), so
    // priority must bypass ordinary background throttling.
    if (isHighPriority_) return true;

    for (auto* other : listVideos) {
        if (other == this || other->videoFile_.empty()) continue;
        if (other->videoInst_ && other->videoInst_->hasError()) continue;

        const bool waiting =
            !other->instanceReady_ ||
            !other->videoInst_ ||
            !other->videoInst_->getTexture();

        if (!waiting) continue;

        // A selected/incoming video outranks all speculative hidden preloads,
        // even while its current alpha is still zero.
        if (other->isHighPriority_) return false;

        // A truly visible video also outranks hidden preloading.
        if (other->checkVisibility()) return false;

        // Admit only one ordinary hidden preroll at a time.
        if (other->instanceReady_) return false;
    }

    return true;
}

void VideoComponent::setHighPriority(bool isHigh) {
    if (isHighPriority_ == isHigh) return;

    const bool promoted = isHigh && !isHighPriority_;
    isHighPriority_ = isHigh;

    if (promoted) {
        // Background work may have accumulated retry delay while this component
        // was off-screen. Once it becomes the selected/destination video, make
        // the very next update eligible to acquire/open immediately.
        pendingVideoRetry_ = false;
        retryAttempts_ = 0;
        nextRetryTime_ = 0;
    }
}

void VideoComponent::setStartupArtwork(const std::string& path) {
    if (path.empty()) { startupArtwork_.reset(); return; }
    if (startupArtwork_ && startupArtwork_->filePath() == path) return;
    startupArtwork_ = std::make_unique<Image>(path, "", page, monitor_);
    startupArtwork_->allocateGraphicsMemory();
}

void VideoComponent::computeDesiredIntent(bool visibleNow, const VideoSnapshot& snap) {
    // 1. Edge detection for "Rewind on Hide". The backend combines the
    // rewind and pause into one ordered async operation, so a retained video
    // is prepared at the beginning while it is off-screen.
    if (!visibleNow && wasVisible_ && hasBeenOnScreen_) {
        desiredState_ = PlaybackTarget::Paused;
        pendingCommand_ = PlaybackCommand::RewindAndPause;
        wasVisible_ = false;
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

    // Dispatch the intended transient command. Hide-time rewind/pause is one
    // backend operation; do not immediately follow it with a second pause job.
    if (pendingCommand_ == PlaybackCommand::RewindAndPause) {
        if (auto* gstVideo = videoInst_.get()) {
            gstVideo->rewindAndPause();
        }
        else {
            // Generic fallback for any future non-GStreamer backend.
            videoInst_->restart();
            videoInst_->pause();
        }

        pendingCommand_ = PlaybackCommand::None;
        return;
    }

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
        // Existing hidden streams can pause normally. New background work waits
        // for visible first frames and is limited to one outstanding preroll.
        if (listId_ != -1 && !instanceReady_ && !canStartBackgroundVideo()) return Component::update(dt);
        if (listId_ == -1 && !videoInst_) return Component::update(dt);
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

void VideoComponent::adoptVideo(std::shared_ptr<IVideo> video) {
    if (!video || videoInst_ == video) return;

    if (videoInst_) {
        if (listId_ != -1) {
            auto old = std::move(videoInst_);
            VideoPool::releaseVideo(std::move(old), monitor_, listId_);
        }
        else {
            videoInst_.reset();
        }
    }

    videoInst_ = std::move(video);
    videoInst_->setSoftOverlay(softOverlay_);

    instanceReady_ = false;
    dimensionsUpdated_ = false;
    hasBeenOnScreen_ = false;
    wasVisible_ = false;
    wasPlayingBeforeFastScroll_ = false;
    desiredState_ = PlaybackTarget::Paused;
    pendingCommand_ = PlaybackCommand::None;

    pendingVideoRetry_ = false;
    retryAttempts_ = 0;
    nextRetryTime_ = 0;
    lastVolume_ = -1.0f;
}

void VideoComponent::freeGraphicsMemory() {
    Component::freeGraphicsMemory();
    startupArtwork_.reset();

    if (!videoInst_) return;

    if (listId_ != -1) {
        auto video = std::move(videoInst_);
        VideoPool::releaseVideo(std::move(video), monitor_, listId_);
        return;
    }

    videoInst_.reset();
}

void VideoComponent::draw() {
    if (!currentPage_) {
        return;
    }


    SDL_Texture* texture = videoInst_ && instanceReady_ ? videoInst_->getTexture() : nullptr;
    if (!texture && startupArtwork_) {
        startupArtwork_->pumpGraphicsPreparation();
        if (startupArtwork_->isGraphicsReadyForFirstRender() && startupArtwork_->baseViewInfo.ImageWidth > 0) {
            baseViewInfo.ImageWidth = startupArtwork_->baseViewInfo.ImageWidth;
            baseViewInfo.ImageHeight = startupArtwork_->baseViewInfo.ImageHeight;
            startupArtwork_->baseViewInfo = baseViewInfo;
            startupArtwork_->draw();
            return;
        }
    }

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
