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

#include <SDL2/SDL.h>
#include <string>
#include <gst/video/video.h>

class IVideo
{
  public:
    virtual ~IVideo() = default;
    virtual bool initialize() = 0;
    virtual bool play(const std::string &file) = 0;
    virtual bool stop() = 0;
    virtual bool deInitialize() = 0;
    virtual SDL_Texture *getTexture() const = 0;
    virtual void draw() = 0;
    virtual void loopHandler() = 0;
    virtual void volumeUpdate() = 0;
    virtual int getHeight() = 0;
    virtual int getWidth() = 0;
    virtual void setVolume(float volume) = 0;
    virtual void skipForward() = 0;
    virtual void skipBackward() = 0;
    virtual void skipForwardp() = 0;
    virtual void skipBackwardp() = 0;
    virtual void pause() = 0;
    virtual void restart() = 0;
    virtual unsigned long long getCurrent() = 0;
    virtual unsigned long long getDuration() = 0;
    virtual bool isPaused() = 0;
    virtual bool isPlaying() = 0;
    virtual void bufferDisconnect(bool disconnect) = 0;
    virtual bool isBufferDisconnected() = 0;

    virtual GstClockTime getLastPTS() const = 0;
    virtual GstClockTime getExpectedTime() const = 0;
    virtual bool isNewFrameAvailable() const = 0;
    virtual void resetNewFrameFlag() = 0;
    virtual GstElement* getPipeline() const = 0;
    virtual GstElement* getVideoSink() const = 0;

};
