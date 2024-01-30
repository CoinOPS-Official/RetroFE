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
#include "Image.h"
#include "../Page.h"
#include "../../Collection/Item.h"
#include "../../Video/IVideo.h"
#include "../../Video/VideoFactory.h"
#include <SDL2/SDL.h>
#include <string>

class VideoComponent : public Component
{
public:
    VideoComponent(Page &p, const std::string& videoFile, int monitor, int numLoops);
    virtual ~VideoComponent();
    bool update(float dt) override;
    void draw() override;
    void freeGraphicsMemory() override;
    void allocateGraphicsMemory() override;
    bool isPlaying() override;
    void skipForward( ) override;
    void skipBackward( ) override;
    void skipForwardp( ) override;
    void skipBackwardp( ) override;
    void pause( ) override;
    void restart( ) override;
    unsigned long long getCurrent( ) override;
    unsigned long long getDuration( ) override;
    bool isPaused( ) override;
    std::string filePath() override;

private:
    std::string videoFile_;
    std::string name_;
    IVideo* videoInst_{ nullptr };
    bool isPlaying_{ false };
    bool hasBeenOnScreen_{ false };
    int numLoops_;
    int monitor_;
};
