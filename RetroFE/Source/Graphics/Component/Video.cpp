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

#include "Video.h"
#include "VideoComponent.h"
#include "VideoBuilder.h"
#include "../../Video/IVideo.h"
#include "../../Video/GStreamerVideo.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include <filesystem>


bool Video::enabled_ = true;


Video::Video(const std::string& file, const std::string& altFile, int numLoops, Page &p, int monitor)
    : Component(p)
    , file_(file)
    , altFile_(altFile)
    , numLoops_(numLoops)

{
    baseViewInfo.Monitor = monitor;
    baseViewInfo.Layout = p.getCurrentLayout();
}

Video::~Video( )
{
    if (video_ != nullptr)
    {
        Component::freeGraphicsMemory();
        delete video_;
        video_ = nullptr;
        Logger::write(Logger::ZONE_DEBUG, "Video", "Deleted " + Utils::getFileName(file_));

    }
}


void Video::setEnabled(bool enabled)
{
    enabled_ = enabled;
}


bool Video::update(float dt)
{
    if(video_)
    {
        // video needs to run a frame to start getting size info
        if(baseViewInfo.ImageHeight == 0 && baseViewInfo.ImageWidth == 0)
        {
            baseViewInfo.ImageWidth = video_->baseViewInfo.ImageWidth;
            baseViewInfo.ImageHeight = video_->baseViewInfo.ImageHeight;
        }
        video_->update(dt);
    }
    
    return Component::update(dt);
}


void Video::freeGraphicsMemory( )
{
    Component::freeGraphicsMemory();
    if (video_)
    {
        video_->freeGraphicsMemory();
        video_ = nullptr;
        Logger::write(Logger::ZONE_DEBUG, "Video", "Deleted " + Utils::getFileName(file_));

    }
}


void Video::allocateGraphicsMemory()
{
    Component::allocateGraphicsMemory();

    if (enabled_ && !video_) {
        std::filesystem::path file;

        // First, check if file_ exists and is not a directory
        if (std::filesystem::exists(file_) && !std::filesystem::is_directory(file_)) {
            file = file_;
        }
        // If file_ was not found or is a directory, check for altFile_
        else if (std::filesystem::exists(altFile_) && !std::filesystem::is_directory(altFile_)) {
            file = altFile_;
        }

        // If a valid file path was found, create a new VideoComponent
        if (!file.empty()) {
            video_ = new VideoComponent(page, file.string(), baseViewInfo.Monitor, numLoops_);
        }
    }

    // If video_ was created, allocate graphics memory for it
    if (video_) {
        video_->allocateGraphicsMemory();
    }
}


std::string Video::filePath()
{
    return file_;
}

void Video::draw( )
{
    Component::draw( );
    if(video_)
    {
    	baseViewInfo.ImageHeight = video_->baseViewInfo.ImageHeight;
    	baseViewInfo.ImageWidth  = video_->baseViewInfo.ImageWidth;
        video_->baseViewInfo     = baseViewInfo;
        video_->draw( );
    }
}


bool Video::isPlaying( )
{
    if (video_)
    {
        return video_->isPlaying( );
    }
    else
    {
        return false;
    }
}
