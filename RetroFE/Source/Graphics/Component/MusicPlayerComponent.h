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
#include "../../Sound/MusicPlayer.h"
#include <string>
#include <vector>

class Configuration;
class Image;
class FontManager;

class MusicPlayerComponent : public Component
{
public:
    MusicPlayerComponent(Configuration& config, bool commonMode, const std::string& type, Page& p, int monitor, FontManager* font = nullptr);
    ~MusicPlayerComponent() override;

    bool update(float dt) override;
    void draw() override;
    void drawAlbumArt();
    SDL_Texture* loadDefaultAlbumArt();
    void drawVolumeBar();
    void freeGraphicsMemory() override;
    void allocateGraphicsMemory() override;
    std::string_view filePath() override; // Add to match other components

    // Control functions for interacting with the music player
    void skipForward() override;
    void skipBackward() override;
    void skipForwardp() override;
    void skipBackwardp() override;
    void pause() override;
    void restart() override;
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;
    bool isPlaying() override;

    // Set the component type
    void setType(const std::string& type) { type_ = type; }

private:
    // Find and load appropriate component based on type and state
    Component* reloadComponent();
    Page* currentPage_;
    Configuration& config_;
    bool commonMode_;
    Component* loadedComponent_;
    std::string type_; // Type of MusicPlayer component: "state", "shuffle", "loop", etc.
    MusicPlayer* musicPlayer_;
    FontManager* font_; // Font for text display

    // State tracking
    std::string lastState_;  // Tracks the last state (playing/paused/etc.)
    float refreshInterval_;  // How often to update in seconds
    float refreshTimer_;

    // Album art tracking
    SDL_Texture* albumArtTexture_;
    int albumArtTrackIndex_;
    SDL_Renderer* renderer_;
    int albumArtTextureWidth_;
    int albumArtTextureHeight_;
    bool isAlbumArt_;

    // Volume bar textures and data
    SDL_Texture* volumeEmptyTexture_;
    SDL_Texture* volumeFullTexture_;
    SDL_Texture* volumeBarTexture_;
    int volumeBarWidth_;
    int volumeBarHeight_;
    int lastVolumeValue_;
    bool isVolumeBar_;

    // Create a volume bar texture based on current volume
    void loadVolumeBarTextures();
    void updateVolumeBarTexture();

    // Alpha animation for volume bar
    float currentDisplayAlpha_; // Current display alpha (for fading)
    float targetAlpha_;         // Target alpha to fade towards
    float fadeSpeed_;           // How fast alpha changes (units per second)
    float volumeStableTimer_;   // How long volume has been stable
    float volumeFadeDelay_;     // How long to wait before fading out
    bool volumeChanging_;       // Is volume currently changing

};