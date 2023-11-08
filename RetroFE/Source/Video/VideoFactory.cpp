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
#include "GStreamerVideo.h"

bool VideoFactory::enabled_ = true;
int VideoFactory::numLoops_ = 0;
IVideo *VideoFactory::instance_ = nullptr;


IVideo* VideoFactory::createVideo(int monitor, int numLoops) 
{
    if (!enabled_) {
        return nullptr; // Early return if not enabled
    }

    auto* instance = new GStreamerVideo(monitor);
    instance->initialize();

    int loopsToSet = (numLoops > 0) ? numLoops : numLoops_;
    instance->setNumLoops(loopsToSet);

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