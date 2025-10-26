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
#include "ImageBuilder.h"
#include "../../Utility/Utils.h"
#include "../../Utility/Log.h"
#include <string_view>

#ifdef WIN32
static constexpr std::string_view kImgExts[] = { "gif","webp","png","jpg","jpeg" };
#else
static constexpr std::string_view kImgExts[] =
{ "gif","GIF","webp","WEBP","png","PNG","jpg","JPG","jpeg","JPEG" };
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

Image* ImageBuilder::CreateImage(const std::string& path, Page& p,
    const std::string& name, int monitor,
    bool additive, bool useTextureCaching) {
    Image* image = nullptr;
    const std::string prefix = makePrefix(path, name);

    std::string file;
    if (Utils::findMatchingFile(std::string_view(prefix),
        std::begin(kImgExts), std::end(kImgExts),
        file)) {
        image = new Image(file, "", p, monitor, additive, useTextureCaching);
    }
    return image;
}

bool ImageBuilder::RetargetImage(Image& img, const std::string& path, const std::string& name) {
    const std::string prefix = makePrefix(path, name);
    std::string found;
    if (Utils::findMatchingFile(std::string_view(prefix),
        std::begin(kImgExts), std::end(kImgExts),
        found)) {
        img.retarget(found, "");
        return true;
    }
    return false;
}

