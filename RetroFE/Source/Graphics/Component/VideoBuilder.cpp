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
#include <string_view>

#ifdef WIN32
static constexpr std::string_view kVidExts[] = {
    "mp4","avi","mkv"
};
#else
static constexpr std::string_view kVidExts[] = {
    "mp4","MP4","avi","AVI","mkv","MKV"
};
#endif

static inline std::string makePrefix(const std::string& path, const std::string& name) {
    std::string s;
    s.reserve(path.size() + name.size() + 2);
    s.append(path);
    if (!path.empty()) {
        const char c = path.back();
        if (c != '/' && c != '\\')
#ifdef _WIN32
            s.push_back('\\');
#else
            s.push_back('/');
#endif
    }
    s.append(name);
    return s;
}

VideoComponent* VideoBuilder::createVideo(const std::string& path, Page& page,
    const std::string& name, int monitor, int numLoops, bool softOverlay,
    int listId, const int* perspectiveCorners) {
    VideoComponent* component = nullptr;

    const std::string prefix = makePrefix(path, name);
    std::string file;
    if (Utils::findMatchingFile(std::string_view(prefix),
        std::begin(kVidExts), std::end(kVidExts),
        file)) {
        component = new VideoComponent(page, file, monitor, numLoops, softOverlay, listId, perspectiveCorners);
    }
    return component;
}

bool VideoBuilder::RetargetVideo(VideoComponent& comp,
    const std::string& directory,
    const std::string& stem) {
    if (directory.empty() || stem.empty()) return false;

    const std::string prefix = makePrefix(directory, stem);
    std::string found;
    if (!Utils::findMatchingFile(std::string_view(prefix),
        std::begin(kVidExts), std::end(kVidExts),
        found))
        return false;

    comp.retarget(found);     // existing non-blocking retarget behavior
    return true;
}