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
#include "IVideo.h"

#include "../Utility/Log.h"
#include "VideoPool.h"
#include <memory>

bool VideoFactory::enabled_ = true;
int VideoFactory::numLoops_ = 0;

std::shared_ptr<IVideo> VideoFactory::createVideo(int monitor, int numLoops, bool softOverlay, int listId, const int* perspectiveCorners) {
    if (!enabled_) {
        return nullptr;
    }

    // VideoPool::acquireVideo now returns std::shared_ptr<IVideo>
    auto instance = VideoPool::acquireVideo(monitor, listId, softOverlay);
    if (!instance) {
        LOG_ERROR("VideoFactory", "VideoPool failed to provide a video instance.");
        return nullptr;
    }

    if (!instance->initialize()) {
        LOG_ERROR("VideoFactory", "Failed to initialize video from VideoPool");
        VideoPool::releaseVideo(instance, monitor, listId);
        return nullptr;
    }

    instance->setNumLoops((numLoops > 0) ? numLoops : numLoops_);
    instance->setSoftOverlay(softOverlay);
    instance->setPerspectiveCorners(perspectiveCorners);

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

namespace { std::string selectedBackend = "gstreamer"; }
const std::string& VideoFactory::backend() { return selectedBackend; }
bool VideoFactory::setBackend(const std::string& name) {
    if (name == "gstreamer") { selectedBackend = name; return true; }
#ifdef RETROFE_HAVE_FFMPEG
    if (name == "ffmpeg") { selectedBackend = name; return true; }
#endif
    LOG_ERROR("VideoFactory", "Unavailable video backend: " + name);
    return false;
}
