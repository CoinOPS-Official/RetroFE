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
#include "ReloadableText.h"
#include "../../Video/IVideo.h"
#include "../../Collection/Item.h"
#include <SDL2/SDL.h>
#include <string>

class Image;

//todo: this class should aggregate Image, Text, and Video component classes
class ReloadableMedia : public Component
{
public:
    ReloadableMedia(Configuration& config, bool systemMode, bool layoutMode, bool commonMode, [[maybe_unused]] bool menuMode, const std::string& type, const std::string& imageType,
        Page& p, int displayOffset, bool isVideo, Font* font, bool jukebox, int jukeboxNumLoops, int randomSelect);
    ~ReloadableMedia() override;
    bool update(float dt) override;
    void draw() override;
    void freeGraphicsMemory() override;
    void allocateGraphicsMemory() override;
    Component* findComponent(const std::string& collection, const std::string& type, const std::string& basename, std::string_view filepath, bool systemMode, bool isVideo);
    void enableTextFallback_(bool value);
    bool isJukeboxPlaying() override;
    void skipForward() override;
    void skipBackward() override;
    void skipForwardp() override;
    void skipBackwardp() override;
    void pause() override;
    void restart() override;
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;


private:
    Component* reloadTexture();
    Configuration& config_;
    bool systemMode_;
    bool layoutMode_;
    bool commonMode_;
    int randomSelect_;
    Component* loadedComponent_{ nullptr };
    bool isVideo_;
    Font* FfntInst_;
    bool textFallback_{ false };
    std::string type_;
    std::string currentCollection_;
    int displayOffset_;
    std::string imageType_;
    bool jukebox_;
    int  jukeboxNumLoops_;
    int numberOfImages_{ 27 };
    
    static inline const std::vector<std::string> imageExtensions = {
        "png", "PNG", "jpg", "JPG", "jpeg", "JPEG",
    };

    static inline const std::vector<std::string> videoExtensions = {
        "mp4", "MP4", "avi", "AVI", "mkv", "MKV",
        "mp3", "MP3", "wav", "WAV", "flac", "FLAC"
    };

};
