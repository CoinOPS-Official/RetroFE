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
    void triggerImmediateUpdate();
    void draw() override;
    void drawProgressBar();
    void drawAlbumArt();
    SDL_Texture* loadDefaultAlbumArt();
    void drawVolumeBar();
    void freeGraphicsMemory() override;
    void allocateGraphicsMemory() override;
    std::string_view filePath() override; // Add to match other components

    // Control functions for interacting with the music player
    void pause() override;
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
    float directionDisplayTimer_;
    const float directionDisplayDuration_;

    // Album art tracking
    SDL_Texture* albumArtTexture_;
    int albumArtTrackIndex_;
    SDL_Renderer* renderer_;
    int albumArtTextureWidth_;
    int albumArtTextureHeight_;
    bool albumArtNeedsUpdate_;
    bool isAlbumArt_;
    void loadAlbumArt();

    // Volume bar textures and data
    SDL_Texture* volumeEmptyTexture_;
    SDL_Texture* volumeFullTexture_;
    SDL_Texture* volumeBarTexture_;
    int volumeBarWidth_;
    int volumeBarHeight_;
    int lastVolumeValue_;
    bool volumeBarNeedsUpdate_;
    bool isVolumeBar_;

    // Create a volume bar texture based on current volume
    void loadVolumeBarTextures();
    int detectSegmentsFromSurface(SDL_Surface* surface);
    void updateVolumeBarTexture();

    // Alpha animation for volume bar
    float currentDisplayAlpha_; // Current display alpha (for fading)
    float targetAlpha_;         // Target alpha to fade towards
    float fadeSpeed_;           // How fast alpha changes (units per second)
    float volumeStableTimer_;   // How long volume has been stable
    float volumeFadeDelay_;     // How long to wait before fading out
    bool volumeChanging_;       // Is volume currently changing

    // VU meter data and rendering
    bool isVuMeter_;
    int vuBarCount_;
    std::vector<float> vuLevels_;
    std::vector<float> vuPeaks_;
    float vuDecayRate_;
    float vuPeakDecayRate_;
    void drawVuMeter();
    void createVuMeterTextureIfNeeded();
    void updateVuMeterTexture();
    bool parseHexColor(const std::string& hexString, SDL_Color& outColor);
    void updateVuLevels();
    SDL_Texture* vuMeterTexture_; // Target texture for VU meter rendering
    int vuMeterTextureWidth_;
    int vuMeterTextureHeight_;
    bool vuMeterNeedsUpdate_; // Flag to track when texture update is needed

    // VU meter theming
    SDL_Color vuBottomColor_;
    SDL_Color vuMiddleColor_;
    SDL_Color vuTopColor_;
    SDL_Color vuBackgroundColor_;
    SDL_Color vuPeakColor_;
    float vuGreenThreshold_;  // Level threshold for green (0.0-1.0)
    float vuYellowThreshold_; // Level threshold for yellow (0.0-1.0)

    int totalSegments_;
    bool useSegmentedVolume_;

};