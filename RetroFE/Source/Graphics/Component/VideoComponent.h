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
#include <atomic>

class VideoComponent : public Component
{
public:
    explicit VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listID);
    ~VideoComponent() override;
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
    std::string_view filePath() override;

private:
    std::string videoFile_;
    IVideo* videoInst_{ nullptr };
    std::atomic<bool> instanceReady_{false};
    bool hasBeenOnScreen_{ false };
    bool softOverlay_;
    int numLoops_;
    int monitor_;
    int listId_;
    Page* currentPage_{ nullptr };
    GstClockTime previousTime_ = 0;
    GstClockTime previousPTS_ = 0;
    gdouble cumulativeProportion_ = 0.0;
    guint64 proportionSampleCount_ = 0;
};
