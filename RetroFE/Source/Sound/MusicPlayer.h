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

#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <random>
#include <atomic>
#include <deque>
#include <mutex>

#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif
#if __has_include(<SDL2/SDL_image.h>)
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
#include <SDL2_image/SDL_image.h>
#else
#error "Cannot find SDL_image header"
#endif
#include "../Database/Configuration.h"


class MusicPlayerComponent;
class MusicPlayer {
public:
    // Singleton & Basic Setup
    static MusicPlayer* getInstance();
    bool hasStartedPlaying() const;  // Returns true once the first track begins playing

    // Track Metadata Structure
    struct TrackMetadata {
        std::string title;
        std::string artist;
        std::string album;
        std::string year;
        std::string genre;
        std::string comment;
        int trackNumber;

        TrackMetadata() : trackNumber(0) {}
    };


    // Enum for track change direction
    enum class PlaybackState {
        NONE,
        PLAYING,
        PAUSED,
        NEXT,
        PREVIOUS
    };

    void setPlaybackState(PlaybackState state) { playbackState_ = state; }
    PlaybackState getPlaybackState() const { return playbackState_; }

    // Initialization & Shutdown
    bool initialize(Configuration& config);
    void shutdown();

    // Playlist & Folder Loading
    bool loadM3UPlaylist(const std::string& playlistPath);
    void loadMusicFolderFromConfig();
    bool loadMusicFolder(const std::string& folderPath);

    // Playback Control
    bool playMusic(int index = -1, int customFadeMs = -1);  // -1 means use current or random track
    bool pauseMusic(int customFadeMs = -1);
    bool resumeMusic(int customFadeMs = -1);
    bool stopMusic(int customFadeMs = -1);
    bool nextTrack(int customFadeMs = -1);
    bool previousTrack(int customFadeMs = -1);
    bool isPlaying() const;
    bool isPaused() const;
    void changeVolume(bool increase);
    double saveCurrentMusicPosition();
    double getCurrent();    // Current playback position (sec)
    double getDuration();   // Duration of current track (sec)
    std::pair<int, int> getCurrentAndDurationSec();
    bool getButtonPressed();
    int getSampleSize() const;
    void setButtonPressed(bool buttonPressed);

    // Volume & Loop Settings
    void setVolume(int volume);  // 0-128 (SDL_Mixer range)
    void setLogicalVolume(int v);
    int getLogicalVolume();
    int getVolume() const;
    void fadeToVolume(int targetVolume, int customFadeMs = -1);
    void fadeBackToPreviousVolume();
    void setLoop(bool loop);
    bool getLoop() const;

    // Shuffle Controls
    bool shuffle();
    bool setShuffle(bool shuffle);
    bool getShuffle() const;

    // Track Navigation & Identification
    int getCurrentTrackIndex() const;
    int getTrackCount() const;
    std::string getCurrentTrackName() const;
    std::string getCurrentTrackNameWithoutExtension() const;
    std::string getCurrentTrackPath() const;
    std::string getFormattedTrackInfo(int index = -1) const;
    std::string getTrackArtist(int index = -1) const;
    std::string getTrackAlbum(int index = -1) const;

    // Detailed Metadata Access
    const TrackMetadata& getCurrentTrackMetadata() const;
    const TrackMetadata& getTrackMetadata(int index) const;
    size_t getTrackMetadataCount() const;
    std::string getCurrentTitle() const;
    std::string getCurrentArtist() const;
    std::string getCurrentAlbum() const;
    std::string getCurrentYear() const;
    std::string getCurrentGenre() const;
    std::string getCurrentComment() const;
    int getCurrentTrackNumber() const;

    // Track Change State
    bool isFading() const;
    bool hasTrackChanged();
    bool isPlayingNewTrack();

    // Album Art Extraction
    bool getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData);

    // Audio Visualization & Processing
    const std::vector<float>& getAudioLevels() const { return audioLevels_; }
    int getAudioChannels() const { return audioChannels_; }
	int getAudioSampleRate() const { return audioSampleRate_; }
    bool registerVisualizerCallback();
    void unregisterVisualizerCallback();
    bool hasVuMeter() const { return hasVuMeter_; }
    void setHasVuMeter(bool enable) { hasVuMeter_ = enable; }

    void addVisualizerListener(MusicPlayerComponent* listener);
    void removeVisualizerListener(MusicPlayerComponent* listener);

private:
    // Constructors / Destructors
    MusicPlayer();
    ~MusicPlayer();

    PlaybackState playbackState_;

    // Private Helper Functions
    void loadTrack(int index);
    bool readTrackMetadataWithGst(const std::string& filePath, TrackMetadata& metadata);
    bool read_id3v2_tags(const std::string& path, TrackMetadata& meta);
    bool readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const;
    bool parseM3UFile(const std::string& playlistPath);
    bool isValidAudioFile(const std::string& filePath) const;
    void setFadeDuration(int ms);
    int getFadeDuration() const;
    void resetShutdownFlag();
    int getNextTrackIndex();
    static void musicFinishedCallback();
    void onMusicFinished();

    static void postMixCallback(void* udata, Uint8* stream, int len);
    void processAudioData(Uint8* stream, int len);

    bool hasActiveVisualizers_ = false; // Replaces hasVisualizer_ and hasGstVisualizer_

    std::vector<MusicPlayerComponent*> visualizerListeners_;
    std::mutex visualizerMutex_; // To protect access to the listener list.

    // Singleton Instance
    static MusicPlayer* instance_;

    // Configuration & Playback State
    Configuration* config_;
    Mix_Music* currentMusic_;
    std::vector<std::string> musicFiles_;
    std::vector<std::string> musicNames_;
    std::vector<TrackMetadata> trackMetadata_;
    std::vector<int> shuffledIndices_;
    int currentShufflePos_;
    int currentIndex_;
    int volume_;
    int logicalVolume_;
    bool loopMode_;
    bool shuffleMode_;
    std::atomic<bool> isShuttingDown_;
    std::atomic<uint32_t> fadeSerial_;
    std::mt19937 rng_;
    bool isPendingPause_;
    double pausedMusicPosition_;
    bool isPendingTrackChange_;
    int pendingTrackIndex_;
    int fadeMs_;
    int previousVolume_;
    bool buttonPressed_;
    std::string lastCheckedTrackPath_;
    bool hasStartedPlaying_;
    Uint64 lastVolumeChangeTime_;
    Uint64 volumeChangeIntervalMs_;


    // Audio Visualization Members
    bool hasVisualizer_;
    std::vector<float> audioLevels_;
    int audioChannels_;
	int audioSampleRate_;
    bool hasVuMeter_;
    int sampleSize_;  // 1, 2, or 4 bytes per sample

    Configuration* config;
    Mix_Music* currentMusic;
    std::vector<std::string> musicFiles;
    std::vector<std::string> musicNames;
    int currentIndex;
    int volume;
    bool loopMode;
    bool shuffleMode;
    bool isShuttingDown;
    std::mt19937 rng;
};