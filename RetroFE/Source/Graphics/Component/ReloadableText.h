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
#include "Component.h"
#include "Text.h"
#include "../Font.h"
#include "../Page.h"
#include "../../Collection/Item.h"
#include <SDL2/SDL.h>
#include <string>
#include <filesystem>

class ReloadableText : public Component
{
public:
    ReloadableText(std::string type, Page &page, Configuration &config, bool systemMode, FontManager *font, std::string layoutKey, std::string timeFormat, std::string textFormat, std::string singlePrefix, std::string singlePostfix, std::string pluralPrefix, std::string pluralPostfix, std::string location = "");
    virtual ~ReloadableText();
    bool     update(float dt);
    void     draw();
    void     freeGraphicsMemory();
    void     allocateGraphicsMemory();
    void     deInitializeFonts();
    void     initializeFonts();

private:
    bool isInTransition() const;
    void ReloadTexture();
    std::string getTimeSince(std::string sinceTimestamp);

    Configuration &config_;
    bool systemMode_;
    Text *imageInst_;
    std::string type_;
    std::string layoutKey_;
    FontManager *fontInst_;
    std::string timeFormat_;
    std::string textFormat_;
    std::string singlePrefix_;
    std::string singlePostfix_;
    std::string pluralPrefix_;
    std::string pluralPostfix_;
    std::string currentType_;
    std::string currentValue_;
    std::string location_;
    std::string filePath_;
    std::filesystem::file_time_type lastWriteTime_;
    Uint64 lastFileReloadTime_ = 0;
    const Uint64 fileDebounceDuration_ = 1000; // 1 second debounce per instance
};
