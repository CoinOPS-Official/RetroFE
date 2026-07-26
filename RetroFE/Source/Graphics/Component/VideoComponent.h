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
#pragma once

#include "Component.h"
#include <string>
#include <memory>
#include "../../Video/IVideo.h"

class IVideo;
class Page;
struct VideoSnapshot;

enum class PlaybackTarget {
    Paused,
    Playing
};

enum class PlaybackCommand {
    None,
    Restart
};

class VideoComponent : public Component {
public:
    VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listId = -1, const int* perspectiveCorners = nullptr);
    ~VideoComponent() override;

    bool update(float dt) override;
    void draw() override;
    void pumpGraphicsPreparation() override;
    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    std::string_view filePath();

    // Controls
    void skipForward();
    void skipBackward();
    void skipForwardp();
    void skipBackwardp();
    void pause();
    void resume();
    void restart();

    // Properties
    unsigned long long getCurrent();
    unsigned long long getDuration();
    bool isPaused();
    bool isPlaying();
    bool hasFinishedLoops();
    bool hasVideoStream();

    // Pooling / Recycling
    bool recycleAsVideo(const std::string& path, const std::string& /*name*/);
    std::shared_ptr<IVideo> extractVideo();

private:
    std::string videoFile_;
    std::shared_ptr<IVideo> videoInst_;
    Page* currentPage_;

    VideoSnapshot currentSnapshot_;

    int monitor_;
    int listId_;
    int numLoops_;
    bool softOverlay_;

    bool hasPerspective_ = false;
    int perspectiveCorners_[8] = { 0 };
    float lastVolume_ = -1.0f;

    bool dimensionsUpdated_ = false;
    bool instanceReady_ = false;
    // --- Deferred Retry Logic ---
    bool pendingVideoRetry_ = false;
    uint32_t retryAttempts_ = 0;
    uint64_t nextRetryTime_ = 0;
    bool preparationPendingUpdate_ = false;
    bool preparedPipeline_ = false;
    bool preparedVisible_ = false;

    // --- Clean Intent Orchestration State ---
    bool wasVisible_ = false;
    bool hasBeenOnScreen_ = false;
    bool wasPlayingBeforeFastScroll_ = false;

    bool lastLaunchedState_ = false;
    bool lastLaunchedStateInit_ = false;

    PlaybackTarget desiredState_{ PlaybackTarget::Paused };
    PlaybackCommand pendingCommand_{ PlaybackCommand::None };

    // Orchestration Pipeline Helpers
    bool checkVisibility() const;
    void computeDesiredIntent(bool visibleNow, const VideoSnapshot& snap);
    void syncPlaybackIntent(const VideoSnapshot& snap);
    bool preparePipeline_();
};
