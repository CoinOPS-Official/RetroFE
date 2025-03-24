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
#if (__APPLE__)
#include <SDL2_mixer/SDL_mixer.h>
#else
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#endif
#include "../Database/Configuration.h"

class MusicPlayer
{
public:
    // Singleton & Basic Setup
    static MusicPlayer* getInstance();
    bool hasStartedPlaying() const;  // Returns true once the first track begins playing

    // Track Metadata Structure
    struct TrackMetadata
    {
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
    enum class TrackChangeDirection {
        NONE,
        NEXT,
        PREVIOUS
    };

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
    double saveCurrentMusicPosition();
    double getCurrent();    // Current playback position (sec)
    double getDuration();   // Duration of current track (sec)
    bool getButtonPressed();
    void setButtonPressed(bool buttonPressed);

    // Volume & Loop Settings
    void setVolume(int volume);  // 0-128 (SDL_Mixer range)
    int getVolume() const;
    void fadeToVolume(int targetPercent);
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
    TrackChangeDirection getTrackChangeDirection() const;
    bool isFading() const;
    void setTrackChangeDirection(TrackChangeDirection direction);
    bool hasTrackChanged();
    bool isPlayingNewTrack();

    // Album Art Extraction
    bool getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData);

    // Audio Visualization & Processing
    static void postMixCallback(void* udata, Uint8* stream, int len);
    void processAudioData(Uint8* stream, int len);
    const std::vector<float>& getAudioLevels() const { return audioLevels_; }
    int getAudioChannels() const { return audioChannels_; }
    bool registerVisualizerCallback();
    void unregisterVisualizerCallback();
    bool hasVisualizer() const { return hasVisualizer_; }

private:
    // Constructors / Destructors
    MusicPlayer();
    ~MusicPlayer();

    // Private Helper Functions
    void loadTrack(int index);
    bool readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const;
    bool parseM3UFile(const std::string& playlistPath);
    bool isValidAudioFile(const std::string& filePath) const;
    void setFadeDuration(int ms);
    int getFadeDuration() const;
    void resetShutdownFlag();
    int getNextTrackIndex();
    static void musicFinishedCallback();
    void onMusicFinished();

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
    bool loopMode_;
    bool shuffleMode_;
    bool isShuttingDown_;
    std::mt19937 rng_;
    bool isPendingPause_;
    double pausedMusicPosition_;
    bool isPendingTrackChange_;
    int pendingTrackIndex_;
    int fadeMs_;
    int previousVolume_;
    bool buttonPressed_;
    std::string lastCheckedTrackPath_;
    TrackChangeDirection trackChangeDirection_;
    bool hasStartedPlaying_;

    // Audio Visualization Members
    std::vector<float> audioLevels_;
    int audioChannels_;
    bool hasVisualizer_;
    int sampleSize_;  // 1, 2, or 4 bytes per sample
};