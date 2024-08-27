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

Image* ImageBuilder::CreateImage(const std::string& path, const std::string& altPath, Page& p, const std::string& name, int monitor, bool additive) {
    static std::vector<std::string> extensions = {
#ifdef WIN32
        "png", "jpg", "jpeg", "gif"
#else
        "png", "PNG", "jpg", "JPG", "jpeg", "JPEG"
#endif
    };

    std::string prefix = Utils::combinePath(path, name);

    // First, try to find an image in the primary path
    if (std::string file; Utils::findMatchingFile(prefix, extensions, file)) {
        return new Image(file, "", p, monitor, additive);
    }

    // If not found, try the alternative path
    if (!altPath.empty()) {
        if (Utils::findMatchingFile(altPath)) {
            return new Image("", altPath, p, monitor, additive);
        }
    }

    // Return nullptr if no image was found
    return nullptr;
    }
