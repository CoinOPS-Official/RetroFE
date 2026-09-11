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
 */
#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <random>
#include <filesystem>

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

class Configuration;
class MusicPlayerComponent;

class MusicPlayer {
public:
    static MusicPlayer* getInstance();

    struct TrackMetadata {
        std::string title;   // UTF-8
        std::string artist;  // UTF-8
        std::string album;   // UTF-8
        std::string year;    // UTF-8
        std::string genre;   // UTF-8
        std::string comment; // UTF-8
        int trackNumber = 0;
    };

    enum class PlaybackState {
        NONE,
        PLAYING,
        PAUSED,
        NEXT,
        PREVIOUS
    };

    bool initialize(Configuration& config);
    void reinitialize();
    void releaseAudio(); // Call before destroying the shared mixer.
    void onGameLaunchStart();
    void onGameLaunchEnd(bool wasSdlUnloaded);
    void shutdown();
    void pump();

    bool loadM3UPlaylist(const std::string& playlistPath); // playlist text interpreted as UTF-8 in .cpp
    void loadMusicFolderFromConfig();
    bool loadMusicFolder(const std::string& folderPath);

    bool playMusic(int index = -1, int customFadeMs = -1, double position = -1);
    bool pauseMusic(int customFadeMs = -1);
    bool resumeMusic(int customFadeMs = -1);
    bool stopMusic(int customFadeMs = -1);
    bool nextTrack(int customFadeMs = -1);
    bool previousTrack(int customFadeMs = -1);

    bool isPlaying() const;
    bool isPaused() const;
    bool hasStartedPlaying() const;
    bool isFading() const;
    bool hasTrackChanged();
    bool isPlayingNewTrack();
    void setPlaybackState(PlaybackState state) { playbackState_ = state; }
    PlaybackState getPlaybackState() const { return playbackState_; }

    void processAudioData(Uint8* stream, int len);
    const std::vector<float>& getAudioLevels() const { return audioLevels_; }
    int getAudioChannels() const { return audioChannels_; }
    int getAudioSampleRate() const { return audioSampleRate_; }
    bool hasVuMeter() const { return hasVuMeter_; }
    void setHasVuMeter(bool enable) { hasVuMeter_ = enable; }
    int getSampleSize() const;
    void addVisualizerListener(MusicPlayerComponent* listener);
    void removeVisualizerListener(MusicPlayerComponent* listener);

    void changeVolume(bool increase);
    void setVolume(int volume);      // 0-128 (Raw Mixer)
    void setLogicalVolume(int v);    // 0-128 (UI Value -> Curve -> Mixer)
    int getLogicalVolume();
    int getVolume() const;
    void fadeToVolume(int targetLogicalVolume, int customFadeMs = -1);
    void fadeBackToPreviousVolume();
    void setLoop(bool loop);
    bool getLoop() const;

    bool shuffle();
    bool setShuffle(bool shuffle);
    bool getShuffle() const;

    int getCurrentTrackIndex() const;
    int getNextTrackIndex();
    int getTrackCount() const;

    std::string getCurrentTrackName() const;                 // UTF-8 display name
    std::string getCurrentTrackNameWithoutExtension() const; // UTF-8 display name sans extension
    std::filesystem::path getCurrentTrackPath() const;       // full path (native)
    std::string getFormattedTrackInfo(int index = -1) const; // UTF-8

    std::string getTrackArtist(int index = -1) const; // UTF-8
    std::string getTrackAlbum(int index = -1) const;  // UTF-8
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

    bool getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData);

    double saveCurrentMusicPosition();
    double getCurrent();
    double getDuration();
    std::pair<int, int> getCurrentAndDurationSec();

    bool getButtonPressed();
    void setButtonPressed(bool buttonPressed);

    void cancelFade();

private:
    MusicPlayer();
    ~MusicPlayer();

    enum class FinishEvent : uint8_t {
        None = 0,
        PauseAfterFade,
        TrackChangeAfterFade,
        StopAfterFade,
        NaturalEnd
    };

    struct MusicTransitionFade {
        bool active = false;
        bool fadingOut = false;
        uint64_t startTimeMs = 0;
        int durationMs = 0;
        int startVol = 0;
        int targetVol = 0;

        FinishEvent finishAction = FinishEvent::None;
        int finishIndex = -1;
        double finishSeekPos = -1.0;
        int fadeInMs = 0;
    };

    void loadTrack(int index);

    // Use fs::path everywhere for I/O to preserve Unicode on Windows.
    bool readTrackMetadata(const std::filesystem::path& filePath, TrackMetadata& metadata) const;
    bool parseM3UFile(const std::filesystem::path& playlistPath);
    bool isValidAudioFile(const std::filesystem::path& filePath) const;

    static void SDLCALL musicFinishedCallback(void*, MIX_Track*);
    int applyVolumeCurve(int logicalVolume) const;
    void beginFadeOutToAction(FinishEvent action, int index, double seekPos, int fadeOutMs, int fadeInMs);
    void beginFadeInToSteadyVolume(int fadeInMs);
    int steadyMusicVolume() const;

    static MusicPlayer* instance_;
    Configuration* config_;

    MIX_Audio* currentMusic_;
    MIX_Track* musicTrack_ = nullptr;
    bool ensureAudio();
    void haltTrack();
    int musicVolume(int volume = -1);
    bool startTrack(double position = -1);

    // Keep paths as fs::path; display names as UTF-8 strings.
    std::vector<std::filesystem::path> musicFiles_;
    std::vector<std::string> musicNames_; // UTF-8 display names
    std::vector<TrackMetadata> trackMetadata_;

    std::vector<int> shuffledIndices_;
    int currentShufflePos_;
    std::mt19937 rng_;

    PlaybackState playbackState_;
    int currentIndex_;
    bool loopMode_;
    bool shuffleMode_;
    bool hasStartedPlaying_;
    int fadeMs_;

    // Path compare should be done with fs::path (normalize in .cpp if needed).
    std::filesystem::path lastCheckedTrackPath_;

    int savedTrackIndex_;
    double savedPosition_;

    std::atomic<int> volume_;
    std::atomic<int> logicalVolume_;
    std::atomic<int> previousLogicalVolume_;

    bool isVolumeFading_ = false;
    uint64_t volumeFadeStartTime_ = 0;
    int volumeFadeDuration_;
    int volumeFadeStartVal_;
    int volumeFadeTargetVal_;
    Uint64 lastVolumeChangeTime_;
    Uint64 volumeChangeIntervalMs_;
    bool buttonPressed_;

    MusicTransitionFade musicFade_;
    std::atomic<FinishEvent> finishEvent_{ FinishEvent::None };
    std::atomic<bool> isShuttingDown_;

    std::vector<MusicPlayerComponent*> visualizerListeners_;
    std::mutex visualizerMutex_;
    bool hasActiveVisualizers_ = false;
    std::vector<float> audioLevels_;
    int audioChannels_;
    int audioSampleRate_;
    bool hasVuMeter_;
    int sampleSize_;
};