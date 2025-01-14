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

// Include your VideoPool header
#include "VideoPool.h"

bool VideoFactory::enabled_ = true;
int VideoFactory::numLoops_ = 0;

IVideo* VideoFactory::createVideo(int monitor, int numLoops, bool softOverlay, int listId)
{
    if (!enabled_) {
        return nullptr; // Early return if not enabled
    }

    // Acquire from the VideoPool instead of directly creating a new GStreamerVideo
    GStreamerVideo* instance = VideoPool::acquireVideo(monitor, listId, softOverlay);
    if (!instance) {
        // Safety check in case acquireVideo somehow returned null
        LOG_ERROR("VideoFactory", "VideoPool failed to provide a GStreamerVideo instance.");
        return nullptr;
    }

    // Optionally re-initialize the instance in case it was previously unloaded
    if (!instance->initialize()) {
        LOG_ERROR("VideoFactory", "Failed to initialize GStreamerVideo from VideoPool");
        // If initialization fails, you can destroy the instance or handle it differently
        delete instance; 
        return nullptr;
    }

    // Determine loops
    int loopsToSet = (numLoops > 0) ? numLoops : numLoops_;
    instance->setNumLoops(loopsToSet);
    instance->setSoftOverlay(softOverlay);

    return instance; // Return as IVideo pointer
}

void VideoFactory::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

void VideoFactory::setNumLoops(int numLoops)
{
    numLoops_ = numLoops;
}
