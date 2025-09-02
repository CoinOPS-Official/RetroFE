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
#include "../../Sound/VuMeterDSP.h"
#include "kiss_fft.h"
#include "kiss_fftr.h"
#include <gst/gst.h>
#include <string>
#include <vector>

class Configuration;
class Image;
class FontManager;

constexpr int FFT_SIZE = 512; // Must be power of 2, matches NR_OF_FREQ*2
constexpr int NR_OF_FREQ = 128; // Number of frequency bands for ISO visualization

class MusicPlayerComponent : public Component {
public:
    MusicPlayerComponent(Configuration& config, bool commonMode, const std::string& type, Page& p, int monitor, FontManager* font = nullptr);
    ~MusicPlayerComponent() override;

    bool update(float dt) override;
    bool updateIsoFFT(); // Change return type
    void draw() override;
    void createGstPipeline();
    void updateGstTextureFromAppSink();
    void drawAlbumArt();
    SDL_Texture* loadDefaultAlbumArt();
    void drawVolumeBar();
    void freeGraphicsMemory() override;
    void allocateGraphicsMemory() override;
    std::string_view filePath() override; // Add to match other components

    void onPcmDataReceived(const Uint8* data, int len);

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
    VUMeterDSP vuDSP_;

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

    // Progress bar
    bool isProgressBar_;
    SDL_Texture* progressBarTexture_;
    int progressBarTextureWidth_;
    int progressBarTextureHeight_;
    bool progressBarNeedsUpdate_;
    float lastProgressPercent_; // To track changes in progress

    void createProgressBarTextureIfNeeded();
    void updateProgressBarTexture();
    void drawGstTexture();
    void drawProgressBarTexture(); // Renamed from the content of the old drawProgressBar



    // Create a volume bar texture based on current volume
    void loadVolumeBarTextures();
    void pushToGst(const Uint8* data, int len);
    int detectSegmentsFromSurface(SDL_Surface* surface);
    void updateVolumeBarTexture();

    // Alpha animation for volume bar
    float currentDisplayAlpha_; // Current display alpha (for fading)
    float targetAlpha_;         // Target alpha to fade towards
    float fadeSpeed_;           // How fast alpha changes (units per second)
    float volumeStableTimer_;   // How long volume has been stable
    float volumeFadeDelay_;     // How long to wait before fading out
    bool volumeChanging_;       // Is volume currently changing

    enum class GStreamerVisType {
        None,
        Goom,
        Wavescope,
        Synaescope,
        Spectrascope
    };
    
    GStreamerVisType gstreamerVisType_ = GStreamerVisType::None;

    GstElement* gstPipeline_{ nullptr };
    GstElement* gstAppSrc_{ nullptr };
    GstElement* gstAppSink_{ nullptr };
    SDL_Texture* gstTexture_{ nullptr };
    int gstTexW_{ 0 };
    int gstTexH_{ 0 };
    std::mutex gstMutex_; // To guard buffer exchange

    int totalSegments_;
    bool useSegmentedVolume_;

    bool isFftVisualizer_ = false;
    kiss_fftr_cfg kissfft_cfg_{ nullptr }; // Use the REAL FFT config type
    std::vector<float> pcmBuffer_;           // PCM mono float buffer
    std::vector<float> pcmBufferLeft_, pcmBufferRight_; // Only needed for stereo VU meter
    std::vector<float> fftBars_;
    std::vector<kiss_fft_cpx> fftOutput_;
    SDL_Texture* fftTexture_ = nullptr;
    SDL_Texture* gradientTexture_ = nullptr;
    int fftTexW_ = 0, fftTexH_ = 0;

    struct VuMeterConfig {
        int barCount{ 12 };
        float decayRate{ 2.0f };
        float peakDecayRate{ 3.0f };
        bool isMono{ false };
        float greenThreshold{ 0.4f };
        float yellowThreshold{ 0.6f };
        float amplification{ 2.0f };  // Default multiplier
        float curvePower{ 4.0f };   // Default exponent for the response curve
        SDL_Color bottomColor{ 0, 220, 0, 255 };
        SDL_Color middleColor{ 220, 220, 0, 255 };
        SDL_Color topColor{ 220, 0, 0, 255 };
        SDL_Color backgroundColor{ 40, 40, 40, 255 };
        SDL_Color peakColor{ 255, 255, 255, 255 };
    };

    void loadVuMeterConfig(); // Helper to load config into the struct
    void drawVuMeterToTexture();

    // --- VU Meter State (separate from config) ---
    VuMeterConfig vuMeterConfig_;
	bool isVuMeter_;
    int vuMeterTextureWidth_{ 0 };
    int vuMeterTextureHeight_{ 0 };
    bool vuMeterNeedsUpdate_{ true };
    std::vector<float> vuLevels_;
    std::vector<float> vuPeaks_;

    bool isIsoVisualizer_;
    bool isoNeedsUpdate_{ false };
    std::vector<float> fftMagnitudes_;
    static constexpr int ISO_HISTORY = 20;
    struct IsoGridRow {
        // Each coordinate is now a contiguous array of floats.
        std::vector<float> x;
        std::vector<float> y;
        std::vector<float> z;
    };
    std::vector<IsoGridRow> iso_grid_;
    struct ProjectedGridRow { std::vector<float> x, y, z_amp; };
    std::vector<ProjectedGridRow> td_grid_;
    float iso_scroll_offset_ = 0.0f;      // Progress (0.0 to 1.0) for the current scroll
    float iso_scroll_rate_ = 30.0f;        // Constant speed: 1.0 = one row scrolls by per second
    float iso_beat_pulse_ = 0.0f;

    // --- Helper methods for the iso visualizer ---
    void updateIsoState(float dt);
    void updateVuMeterFFT(float dt);
    bool fillPcmBuffer();
    void drawIsoVisualizer(SDL_Renderer* renderer, int win_w, int win_h);

    std::deque<std::vector<Uint8>> pcmQueue_;
    std::mutex pcmMutex_;

    std::vector<float> fftSmoothing_; // size NR_OF_FREQ

    // *** NEW: Unified FFT visualizer check ***
    inline bool isFftVisualizer() const { return isIsoVisualizer_ || isVuMeter_; }

    };