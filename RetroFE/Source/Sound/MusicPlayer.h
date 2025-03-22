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
#include <mutex>
#include <gst/gst.h>
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

    enum class TrackChangeDirection {
        NONE,
        NEXT,
        PREVIOUS
    };

    TrackChangeDirection getTrackChangeDirection() const;

    bool isFading() const;

    const TrackMetadata& getCurrentTrackMetadata() const;
    const TrackMetadata& getTrackMetadata(int index) const;
    size_t getTrackMetadataCount() const;

    // Direct accessors for current track's metadata fields
    std::string getCurrentTitle() const;
    std::string getCurrentArtist() const;
    std::string getCurrentAlbum() const;
    std::string getCurrentYear() const;
    std::string getCurrentGenre() const;
    std::string getCurrentComment() const;
    int getCurrentTrackNumber() const;

    bool initialize(Configuration& config);
    bool loadM3UPlaylist(const std::string& playlistPath);
    void loadMusicFolderFromConfig();
    bool loadMusicFolder(const std::string& folderPath);
    bool playMusic(int index = -1, int customFadeMs = -1);  // -1 means play current or random track
    double saveCurrentMusicPosition();
    bool pauseMusic(int customFadeMs = -1);
    bool resumeMusic(int customFadeMs = -1);
    bool stopMusic(int customFadeMs = -1);
    bool nextTrack(int customFadeMs = -1);
    bool previousTrack(int customFadeMs = -1);
    bool isPlaying() const;
    bool isPaused() const;
    void setVolume(int volume);
    int getVolume() const;
    std::string getCurrentTrackName() const;
    std::string getCurrentTrackNameWithoutExtension() const;
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

    bool hasTrackChanged();
    bool isPlayingNewTrack();
    bool getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData);
    double getCurrent();
    double getDuration();
    void setTrackChangeDirection(TrackChangeDirection direction);

private:
    MusicPlayer();
    ~MusicPlayer();

    std::vector<TrackMetadata> trackMetadata_;
    TrackChangeDirection trackChangeDirection_;

    // GStreamer message handling
    static GstBusSyncReply busCallback(GstBus* bus, GstMessage* message, gpointer data);
    void handleMessage(GstMessage* message);
    void onEos();

    // Track handling
    int getNextTrackIndex();
    void loadTrack(int index);
    bool readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const;
    bool parseM3UFile(const std::string& playlistPath);
    bool isValidAudioFile(const std::string& filePath) const;

    static MusicPlayer* instance_;

    // GStreamer elements
    GstElement* pipeline_;
    GstElement* source_;
    GstElement* volumeElement_; // Renamed to avoid conflict
    GstBus* bus_;

    // State and configuration
    Configuration* config_;
    std::vector<std::string> musicFiles_;
    std::vector<std::string> musicNames_;
    std::vector<int> shuffledIndices_;
    int currentShufflePos_ = -1;
    int currentIndex_;
    int volumeLevel_; // Renamed to avoid conflict
    bool loopMode_;
    bool shuffleMode_;
    bool isShuttingDown_;
    std::mt19937 rng_;
    std::string lastCheckedTrackPath_;

    // Tag extraction via message bus
    GstTagList* pendingTags_;
    int pendingTagTrackIndex_;
    std::mutex tagMutex_;
    bool extractMetadataFromTagList(GstTagList* tags, TrackMetadata& metadata) const;
};