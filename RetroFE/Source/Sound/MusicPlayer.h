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
#if (__APPLE__)
#include <SDL2_mixer/SDL_mixer.h>
#else
#include <SDL2/SDL_mixer.h>
#endif
#include "../Database/Configuration.h"

class MusicPlayer
{
public:
    static MusicPlayer* getInstance();
    struct TrackMetadata
    {
        std::string title;
        std::string artist;
        std::string album;
        std::string year;
        std::string genre;
        std::string comment;
        int trackNumber;

        // Constructor with default values
        TrackMetadata() : trackNumber(0) {}
    };
    bool initialize(Configuration& config);
    bool loadMusicFolder(const std::string& folderPath);
    bool playMusic(int index = -1);  // -1 means play current or random track
    bool pauseMusic();
    bool resumeMusic();
    bool stopMusic();
    bool nextTrack();
    bool previousTrack();
    bool isPlaying() const;
    bool isPaused() const;
    void setVolume(int volume);  // 0-128 (SDL_Mixer range)
    int getVolume() const;
    std::string getCurrentTrackName() const;
    std::string getCurrentTrackPath() const;
    std::string getFormattedTrackInfo(int index = -1) const;
    std::string getTrackArtist(int index = -1) const;
    std::string getTrackAlbum(int index = -1) const;
    int getCurrentTrackIndex() const;
    int getTrackCount() const;
    void setLoop(bool loop);
    bool getLoop() const;
    bool shuffle();
    bool setShuffle(bool shuffle);
    bool getShuffle() const;
    void shutdown();

private:
    MusicPlayer();
    ~MusicPlayer();


    std::vector<TrackMetadata> trackMetadata;

    static void musicFinishedCallback();
    void onMusicFinished();
    void resetShutdownFlag();
    int getNextTrackIndex();
    void loadTrack(int index);
    bool readTrackMetadata(const std::string& filePath, TrackMetadata& metadata);
    static MusicPlayer* instance;

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