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

#include <SDL3/SDL.h>
#include <string>
#include <cstdint>

struct VideoDim {
    int w = -1;
    int h = -1;
};

// 1. Move the enum OUTSIDE the class so the struct can see it
enum class VideoState {
    None,
    Ready,
    Playing,
    Paused
};

// 2. Now the struct can safely use the enum
struct VideoSnapshot {
    VideoState targetState = VideoState::None;
    VideoState actualState = VideoState::None;
    bool pipelineReady = false;
    bool hasError = false;
    bool hasFinishedLoops = false;
    bool hasVideoStream = true;
};

class IVideo {
public:
    // To avoid breaking existing code that uses IVideo::VideoState, 
    // we can alias it back inside the class
    using VideoState = ::VideoState;

    virtual ~IVideo() = default;

    // --- Core Interface ---
    virtual bool initialize() = 0;
    virtual bool deInitialize() = 0;

    // Changed play() to open() as part of the orchestration refactor
    virtual bool open(const std::string& file) = 0;
    virtual bool unload() = 0;
    virtual bool stop() = 0;

    // --- Snapshot & State ---
    virtual VideoSnapshot getSnapshot() const = 0;
    virtual VideoState getTargetState() const = 0;
    virtual VideoState getActualState() const = 0;
    virtual bool isPipelineReady() const = 0;
    virtual bool hasError() const = 0;
    virtual bool hasFinishedLoops() const = 0;
    virtual bool hasVideoStream() const { return true; } // Default to true for non-Gst videos

    // --- Rendering & Media ---
    virtual SDL_Texture* getTexture() const = 0;
    virtual void updateFrame() = 0; // Renamed from draw()
    virtual VideoDim getDimensions() = 0;
    virtual void setSoftOverlay(bool value) = 0;
    virtual void setVolume(float volume) = 0;

    // --- Playback Controls ---
    virtual void skipForward() = 0;
    virtual void skipBackward() = 0;
    virtual void skipForwardp() = 0;
    virtual void skipBackwardp() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void restart() = 0;
    virtual void loop() = 0;

    // --- Properties ---
    virtual unsigned long long getCurrent() = 0;
    virtual unsigned long long getDuration() = 0;
    virtual bool isPaused() = 0;
    virtual bool isPlaying() = 0;
};