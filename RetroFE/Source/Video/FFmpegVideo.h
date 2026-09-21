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
#include "IVideo.h"
#include <memory>

// Control/render methods run on the main thread; demux/decode is worker-owned.
class FFmpegVideo final : public IVideo {
  public:
    explicit FFmpegVideo(int monitor);
    ~FFmpegVideo() override;
    bool initialize() override;
    bool deInitialize() override;
    bool open(const std::string &) override;
    bool unload() override;
    bool stop() override;
    bool prepareForRetarget() override;
    bool isReadyForReuse() const override;
    VideoSnapshot getSnapshot() const override;
    VideoState getTargetState() const override;
    VideoState getActualState() const override;
    bool isPipelineReady() const override;
    bool hasError() const override;
    bool hasFinishedLoops() const override;
    bool hasVideoStream() const override;
    SDL_Texture *getTexture() const override;
    bool usingGpuTexture() const override;
    uint64_t gpuFrameCount() const override;
    void updateFrame() override;
    VideoDim getDimensions() override;
    void setSoftOverlay(bool) override;
    void setVolume(float) override;
    void setNumLoops(int) override;
    void setPerspectiveCorners(const int *) override;
    void skipForward() override;
    void skipBackward() override;
    void skipForwardp() override;
    void skipBackwardp() override;
    void pause() override;
    void resume() override;
    void restart() override;
    void rewindAndPause() override;
    void loop() override;
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;
    bool isPlaying() override;

  private:
    void seek(int64_t nanoseconds, bool paused);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
