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

#include "Image.h"
#include "VideoComponent.h"
#include "../Page.h"
#include "../../Video/VideoFactory.h"

class Page;
class VideoComponent;

//todo: this is more of a factory than a builder
class VideoBuilder
{
public:
    static VideoComponent* createVideo(const std::string& path, Page& page, const std::string& name, int monitor, int numLoops = -1, bool softOverlay = false, int listId = -1, const int* perspectiveCorners = nullptr);
};
