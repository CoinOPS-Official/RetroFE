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

#include "VideoFactory.h"

#include "../Utility/Log.h"
#include "GStreamerVideo.h"
#include "VideoPool.h"
#include <memory>

bool VideoFactory::enabled_ = true;
int VideoFactory::numLoops_ = 0;

std::unique_ptr<IVideo> VideoFactory::createVideo(int monitor, int numLoops, bool softOverlay, int listId) {
    if (!enabled_) {
        return nullptr;
    }

    // VideoPool::acquireVideo now returns std::unique_ptr<IVideo>
    auto instance = VideoPool::acquireVideo(monitor, listId, softOverlay);
    if (!instance) {
        LOG_ERROR("VideoFactory", "VideoPool failed to provide a video instance.");
        return nullptr;
    }

    // Since instance is now a unique_ptr, use -> instead of .
    if (!instance->initialize()) {
        LOG_ERROR("VideoFactory", "Failed to initialize video from VideoPool");
        // No need to delete - unique_ptr will handle cleanup
        return nullptr;
    }

    // Cast to GStreamerVideo to access specific methods
    if (auto* gstreamerVid = dynamic_cast<GStreamerVideo*>(instance.get())) {
        int loopsToSet = (numLoops > 0) ? numLoops : numLoops_;
        gstreamerVid->setNumLoops(loopsToSet);
        gstreamerVid->setSoftOverlay(softOverlay);
    }

    // Return the unique_ptr - ownership is transferred to caller
    return instance;
}

void VideoFactory::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

void VideoFactory::setNumLoops(int numLoops)
{
    numLoops_ = numLoops;
}
