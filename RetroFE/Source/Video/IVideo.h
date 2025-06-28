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
#include <functional>

class IVideo {
public:
	enum class VideoState {
		None,
		Playing,
		Paused
	};

	virtual VideoState getTargetState() const = 0;
	virtual VideoState getActualState() const = 0;
	virtual ~IVideo() = default;
	virtual bool initialize() = 0;
	virtual bool play(const std::string& file) = 0;
	virtual bool stop() = 0;
	virtual bool deInitialize() = 0;
	virtual SDL_Texture* getTexture() const = 0;
	virtual void draw() = 0;
	virtual void volumeUpdate() = 0;
	virtual int getHeight() = 0;
	virtual int getWidth() = 0;
	virtual void setVolume(float volume) = 0;
	virtual void skipForward() = 0;
	virtual void skipBackward() = 0;
	virtual void skipForwardp() = 0;
	virtual void skipBackwardp() = 0;
	virtual void pause() = 0;
	virtual void resume() = 0;
	virtual void restart() = 0;
	virtual unsigned long long getCurrent() = 0;
	virtual unsigned long long getDuration() = 0;
	virtual bool isPaused() = 0;
	virtual bool isPlaying() = 0;
	virtual bool isPipelineReady() const = 0;
	virtual bool hasError() const = 0;
	virtual bool unload() = 0;
};
