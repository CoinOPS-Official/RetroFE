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

#include "VideoBuilder.h"
#include "../../Utility/Utils.h"
#include <fstream>


VideoComponent * VideoBuilder::createVideo(const std::string& path, Page &page, const std::string& name, int monitor, int numLoops, bool softOverlay, int listId, const int* perspectiveCorners)
{
    VideoComponent *component = nullptr;
    
    // Declare the extensions vector as static so it's only initialized once.
#ifdef WIN32
    static std::vector<std::string> extensions = {
        "mp4", "avi", "mkv",
        "mp3", "wav", "flac"
    };
#else
    static std::vector<std::string> extensions = {
    "mp4", "MP4", "avi", "AVI", "mkv", "MKV",
    "mp3", "MP3", "wav", "WAV", "flac", "FLAC"
    };
#endif

    std::string prefix = Utils::combinePath(path, name);

    if(std::string file; Utils::findMatchingFile(prefix, extensions, file)) {
        component = new VideoComponent(page, file, monitor, numLoops, softOverlay, listId, perspectiveCorners);
        //component->allocateGraphicsMemory();
    }

    return component;
}

