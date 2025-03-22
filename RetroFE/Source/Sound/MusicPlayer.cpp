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

#include "MusicPlayer.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include <gst/pbutils/pbutils.h>  // For GstDiscoverer
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

MusicPlayer* MusicPlayer::instance_ = nullptr;

MusicPlayer* MusicPlayer::getInstance()
{
    if (!instance_)
    {
        instance_ = new MusicPlayer();
    }
    return instance_;
}

MusicPlayer::MusicPlayer()
    : pipeline_(nullptr)
    , source_(nullptr)
    , volumeElement_(nullptr)
    , bus_(nullptr)
    , config_(nullptr)
    , currentIndex_(-1)
    , volumeLevel_(100)
    , loopMode_(false)
    , shuffleMode_(false)
    , isShuttingDown_(false)
    , trackChangeDirection_(TrackChangeDirection::NONE)
    , pendingTags_(nullptr)
    , pendingTagTrackIndex_(-1) 
{
    // Seed the random number generator with current time
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    uint64_t seed = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    // Create a seed sequence for better randomization
    std::seed_seq seq{
        static_cast<uint32_t>(seed & 0xFFFFFFFF),
        static_cast<uint32_t>((seed >> 32) & 0xFFFFFFFF)
    };

    rng_.seed(seq);

    // Create GStreamer pipeline
    pipeline_ = gst_pipeline_new("music-player");
    if (!pipeline_) {
        LOG_ERROR("MusicPlayer", "Failed to create GStreamer pipeline");
        return;
    }

    // Create playbin element (handles source, decoding, and output)
    source_ = gst_element_factory_make("playbin", "music-source");
    if (!source_) {
        LOG_ERROR("MusicPlayer", "Failed to create GStreamer playbin element");
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return;
    }

    // Add source to pipeline
    gst_bin_add(GST_BIN(pipeline_), source_);

    // Get volume element from playbin
    g_object_get(source_, "volume", &volumeElement_, NULL);

    // Get the bus for the pipeline
    bus_ = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    if (bus_) {
        // Set sync handler for bus messages
        gst_bus_set_sync_handler(bus_, (GstBusSyncHandler)busCallback, this, NULL);

        // Also watch for messages asynchronously
        gst_bus_add_watch(bus_,
            (GstBusFunc)([](GstBus* bus, GstMessage* msg, gpointer data) -> gboolean {
                MusicPlayer* player = static_cast<MusicPlayer*>(data);
                if (player) player->handleMessage(msg);
                return TRUE;
                }),
            this);

        gst_object_unref(bus_);
    }
}

MusicPlayer::~MusicPlayer()
{
    isShuttingDown_ = true;
    stopMusic();

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

bool MusicPlayer::initialize(Configuration& config)
{
    this->config_ = &config;

    // Get volume from config if available
    int configVolume;
    if (config.getProperty("musicPlayer.volume", configVolume))
    {
        volumeLevel_ = std::max(0, std::min(100, configVolume));
        // Apply volume to GStreamer
        setVolume(volumeLevel_);
    }

    // Get loop mode from config
    bool configLoop;
    if (config.getProperty("musicPlayer.loop", configLoop))
    {
        loopMode_ = configLoop;
    }

    // Get shuffle mode from config
    bool configShuffle;
    if (config.getProperty("musicPlayer.shuffle", configShuffle))
    {
        shuffleMode_ = configShuffle;
    }

    // First check if an M3U playlist is specified
    std::string m3uPlaylist;
    if (config.getProperty("musicPlayer.m3uplaylist", m3uPlaylist))
    {
        // If the path is relative, resolve it against RetroFE's path
        if (!fs::path(m3uPlaylist).is_absolute())
        {
            m3uPlaylist = Utils::combinePath(Configuration::absolutePath, m3uPlaylist);
        }

        if (loadM3UPlaylist(m3uPlaylist))
        {
            LOG_INFO("MusicPlayer", "Initialized with M3U playlist: " + m3uPlaylist);
        }
        else
        {
            LOG_WARNING("MusicPlayer", "Failed to load M3U playlist: " + m3uPlaylist + ". Falling back to folder loading.");
            // Fall back to folder loading if playlist loading fails
            loadMusicFolderFromConfig();
        }
    }
    else
    {
        // No M3U playlist specified, use folder loading
        loadMusicFolderFromConfig();
    }

    LOG_INFO("MusicPlayer", "Initialized with volume: " + std::to_string(volumeLevel_) +
        ", loop: " + std::to_string(loopMode_) +
        ", shuffle: " + std::to_string(shuffleMode_) +
        ", tracks found: " + std::to_string(musicFiles_.size()));

    return true;
}

// Helper method to extract the folder loading logic
void MusicPlayer::loadMusicFolderFromConfig()
{
    std::string musicFolder;
    if (config_ && config_->getProperty("musicPlayer.folder", musicFolder))
    {
        loadMusicFolder(musicFolder);
    }
    else
    {
        // Default to a music directory in RetroFE's path
        loadMusicFolder(Utils::combinePath(Configuration::absolutePath, "music"));
    }
}

bool MusicPlayer::loadMusicFolder(const std::string& folderPath)
{
    // Clear existing music files
    musicFiles_.clear();
    musicNames_.clear();
    trackMetadata_.clear();

    LOG_INFO("MusicPlayer", "Loading music from folder: " + folderPath);

    try
    {
        if (!fs::exists(folderPath))
        {
            LOG_WARNING("MusicPlayer", "Music folder doesn't exist: " + folderPath);
            return false;
        }

        std::vector<std::tuple<std::string, std::string, TrackMetadata>> musicEntries;

        for (const auto& entry : fs::directory_iterator(folderPath))
        {
            if (entry.is_regular_file())
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".mod")
                {
                    std::string filePath = entry.path().string();
                    std::string fileName = entry.path().filename().string();

                    TrackMetadata metadata;
                    readTrackMetadata(filePath, metadata);

                    musicEntries.push_back(std::make_tuple(filePath, fileName, metadata));
                }
            }
        }

        // Sort entries - can sort by metadata fields if needed
        std::sort(musicEntries.begin(), musicEntries.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a) < std::get<1>(b);
            });

        // Unpack sorted entries
        for (const auto& entry : musicEntries)
        {
            musicFiles_.push_back(std::get<0>(entry));
            musicNames_.push_back(std::get<1>(entry));
            trackMetadata_.push_back(std::get<2>(entry));
        }

        LOG_INFO("MusicPlayer", "Found " + std::to_string(musicFiles_.size()) + " music files");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("MusicPlayer", "Error scanning music directory: " + std::string(e.what()));
        return false;
    }

    return !musicFiles_.empty();
}

bool MusicPlayer::loadM3UPlaylist(const std::string& playlistPath)
{
    // Clear existing music files
    musicFiles_.clear();
    musicNames_.clear();
    trackMetadata_.clear();

    LOG_INFO("MusicPlayer", "Loading music from M3U playlist: " + playlistPath);

    if (!parseM3UFile(playlistPath))
    {
        LOG_ERROR("MusicPlayer", "Failed to parse M3U playlist: " + playlistPath);
        return false;
    }

    LOG_INFO("MusicPlayer", "Found " + std::to_string(musicFiles_.size()) + " music files in playlist");
    return !musicFiles_.empty();
}

bool MusicPlayer::parseM3UFile(const std::string& playlistPath)
{
    try
    {
        if (!fs::exists(playlistPath))
        {
            LOG_WARNING("MusicPlayer", "M3U playlist file doesn't exist: " + playlistPath);
            return false;
        }

        std::ifstream playlistFile(playlistPath);
        if (!playlistFile.is_open())
        {
            LOG_ERROR("MusicPlayer", "Failed to open M3U playlist: " + playlistPath);
            return false;
        }

        // Get the directory of the playlist for resolving relative paths
        fs::path playlistDir = fs::path(playlistPath).parent_path();
        std::string line;
        std::vector<std::tuple<std::string, std::string, TrackMetadata>> musicEntries;

        while (std::getline(playlistFile, line))
        {
            // Skip empty lines and comments (lines starting with #)
            if (line.empty() || line[0] == '#')
            {
                // Some M3U files use #EXTINF for track info, but we'll ignore that for now
                continue;
            }

            // Process the file path
            fs::path trackPath = line;

            // If the path is relative, resolve it against the playlist directory
            if (!trackPath.is_absolute())
            {
                trackPath = playlistDir / trackPath;
            }

            // Convert to string and normalize
            std::string filePath = trackPath.string();

            // Check if the file exists and is a valid audio file
            if (fs::exists(filePath) && isValidAudioFile(filePath))
            {
                std::string fileName = trackPath.filename().string();

                TrackMetadata metadata;
                readTrackMetadata(filePath, metadata);

                musicEntries.push_back(std::make_tuple(filePath, fileName, metadata));
            }
            else
            {
                LOG_WARNING("MusicPlayer", "Skipping invalid or non-existent track in playlist: " + filePath);
            }
        }

        // Sort entries (same as in loadMusicFolder)
        std::sort(musicEntries.begin(), musicEntries.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a) < std::get<1>(b);
            });

        // Unpack sorted entries
        for (const auto& entry : musicEntries)
        {
            musicFiles_.push_back(std::get<0>(entry));
            musicNames_.push_back(std::get<1>(entry));
            trackMetadata_.push_back(std::get<2>(entry));
        }

        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("MusicPlayer", "Error parsing M3U playlist: " + std::string(e.what()));
        return false;
    }
}

bool MusicPlayer::isValidAudioFile(const std::string& filePath) const
{
    std::string ext = fs::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".mod");
}

// Replace the current loadTrack method with this:
void MusicPlayer::loadTrack(int index)
{
    if (index < 0 || index >= static_cast<int>(musicFiles_.size()))
    {
        LOG_ERROR("MusicPlayer", "Invalid track index: " + std::to_string(index));
        currentIndex_ = -1;
        return;
    }

    // Convert file path to proper URI format
    std::string filePath = musicFiles_[index];
    std::string uri;

    // Check if path is already a URI
    if (filePath.substr(0, 7) == "file://" ||
        filePath.substr(0, 7) == "http://" ||
        filePath.substr(0, 8) == "https://")
    {
        uri = filePath;
    }
    else
    {
        // Convert to absolute path if needed
        fs::path path(filePath);
        if (!path.is_absolute()) {
            path = fs::absolute(path);
        }

        // Create proper URI with correct encoding
        std::string absolutePath = path.string();

        // Make sure path uses forward slashes
        std::replace(absolutePath.begin(), absolutePath.end(), '\\', '/');

        // Handle Windows drive letters (e.g., C:/ -> /C:/)
        if (absolutePath.size() > 2 && absolutePath[1] == ':' && absolutePath[2] == '/') {
            uri = "file:///" + absolutePath;
        }
        else {
            // For Unix-like paths
            uri = "file://" + absolutePath;
        }
    }

    LOG_INFO("MusicPlayer", "Setting URI: " + uri);
    g_object_set(source_, "uri", uri.c_str(), NULL);

    currentIndex_ = index;
    LOG_INFO("MusicPlayer", "Loaded track: " + musicNames_[index]);
}

bool MusicPlayer::readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const
{
    return false;
}

GstBusSyncReply MusicPlayer::busCallback(GstBus* bus, GstMessage* message, gpointer data)
{
    MusicPlayer* player = static_cast<MusicPlayer*>(data);
    if (player) {
        player->handleMessage(message);
    }
    return GST_BUS_PASS;
}

bool MusicPlayer::extractMetadataFromTagList(GstTagList* tags, TrackMetadata& metadata) const
{
    if (!tags) return false;

    bool metadataUpdated = false;

    // Extract title
    gchar* title = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &title)) {
        if (title && title[0] != '\0') {
            metadata.title = title;
            metadataUpdated = true;
        }
        g_free(title);
    }

    // Extract artist
    gchar* artist = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &artist)) {
        if (artist && artist[0] != '\0') {
            metadata.artist = artist;
            metadataUpdated = true;
        }
        g_free(artist);
    }

    // Also check for PERFORMER tag which is sometimes used instead of ARTIST
    if (metadata.artist.empty()) {
        gchar* performer = NULL;
        if (gst_tag_list_get_string(tags, GST_TAG_PERFORMER, &performer)) {
            if (performer && performer[0] != '\0') {
                metadata.artist = performer;
                metadataUpdated = true;
            }
            g_free(performer);
        }
    }

    // Extract album
    gchar* album = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &album)) {
        if (album && album[0] != '\0') {
            metadata.album = album;
            metadataUpdated = true;
        }
        g_free(album);
    }

    // Extract year
    gchar* date_str = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_DATE_TIME, &date_str)) {
        if (date_str && date_str[0] != '\0') {
            // Try to extract just the year from the date string
            if (strlen(date_str) >= 4) {
                metadata.year = std::string(date_str, 4); // Just get YYYY part
            }
            else {
                metadata.year = date_str;
            }
            metadataUpdated = true;
        }
        g_free(date_str);
    }

    // Fallback to DATE tag if DATE_TIME didn't work
    if (metadata.year.empty()) {
        GDate* date = NULL;
        if (gst_tag_list_get_date(tags, GST_TAG_DATE, &date)) {
            if (date && g_date_valid(date)) {
                gchar date_str[16];
                g_date_strftime(date_str, sizeof(date_str), "%Y", date);
                metadata.year = date_str;
                metadataUpdated = true;
            }
            if (date) {
                g_date_free(date);
            }
        }
    }

    // Extract genre
    gchar* genre = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_GENRE, &genre)) {
        if (genre && genre[0] != '\0') {
            metadata.genre = genre;
            metadataUpdated = true;
        }
        g_free(genre);
    }

    // Extract comment
    gchar* comment = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_COMMENT, &comment)) {
        if (comment && comment[0] != '\0') {
            metadata.comment = comment;
            metadataUpdated = true;
        }
        g_free(comment);
    }

    // Try DESCRIPTION if COMMENT isn't available
    if (metadata.comment.empty()) {
        gchar* description = NULL;
        if (gst_tag_list_get_string(tags, GST_TAG_DESCRIPTION, &description)) {
            if (description && description[0] != '\0') {
                metadata.comment = description;
                metadataUpdated = true;
            }
            g_free(description);
        }
    }

    // Extract track number
    guint track_number = 0;
    if (gst_tag_list_get_uint(tags, GST_TAG_TRACK_NUMBER, &track_number)) {
        metadata.trackNumber = track_number;
        metadataUpdated = true;
    }

    return metadataUpdated;
}

void MusicPlayer::handleMessage(GstMessage* message)
{
    // Skip message handling if we're shutting down
    if (isShuttingDown_) {
        return;
    }

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
        // End of stream reached
        onEos();
        break;
    }

    case GST_MESSAGE_TAG: {
        // New tags have been found
        GstTagList* tags = NULL;
        gst_message_parse_tag(message, &tags);

        if (tags) {
            std::lock_guard<std::mutex> lock(tagMutex_);

            // Log a few key tags for debugging
            gchar* title = NULL;
            if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &title)) {
                LOG_INFO("MusicPlayer", "Tag found - Title: " + std::string(title));
                g_free(title);
            }

            // If we already have tags for the current track, merge them
            if (pendingTags_ && pendingTagTrackIndex_ == currentIndex_) {
                LOG_INFO("MusicPlayer", "Merging new tags with existing tags for track: " +
                    std::to_string(currentIndex_));
                gst_tag_list_insert(pendingTags_, tags, GST_TAG_MERGE_PREPEND);
                gst_tag_list_unref(tags);
            }
            // Otherwise, store the new tags
            else {
                if (pendingTags_) {
                    gst_tag_list_unref(pendingTags_);
                }
                pendingTags_ = tags;
                pendingTagTrackIndex_ = currentIndex_;
                LOG_INFO("MusicPlayer", "Storing new tags for track: " +
                    std::to_string(currentIndex_));
            }

            // Update current track's metadata if it matches pendingTagTrackIndex_
            if (pendingTagTrackIndex_ >= 0 &&
                pendingTagTrackIndex_ < static_cast<int>(trackMetadata_.size())) {
                bool metadataUpdated = extractMetadataFromTagList(
                    pendingTags_, trackMetadata_[pendingTagTrackIndex_]);

                if (metadataUpdated) {
                    LOG_INFO("MusicPlayer", "Updated metadata for track: " +
                        getFormattedTrackInfo(pendingTagTrackIndex_));
                }
            }
        }
        break;
    }

    case GST_MESSAGE_ERROR: {
        // Error occurred
        GError* err = NULL;
        gchar* debug_info = NULL;

        gst_message_parse_error(message, &err, &debug_info);
        std::string errorMsg = (err && err->message) ? err->message : "Unknown error";
        std::string debugInfo = debug_info ? debug_info : "No debug info";

        LOG_ERROR("MusicPlayer", "GStreamer error: " + errorMsg);
        LOG_ERROR("MusicPlayer", "Debug details: " + debugInfo);

        g_clear_error(&err);
        g_free(debug_info);
        break;
    }

    case GST_MESSAGE_WARNING: {
        // Warning message
        GError* err = NULL;
        gchar* debug_info = NULL;

        gst_message_parse_warning(message, &err, &debug_info);
        std::string warningMsg = (err && err->message) ? err->message : "Unknown warning";

        LOG_WARNING("MusicPlayer", "GStreamer warning: " + warningMsg);

        g_clear_error(&err);
        g_free(debug_info);
        break;
    }

    case GST_MESSAGE_STATE_CHANGED: {
        // Only log state changes for our pipeline
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline_)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);

            LOG_INFO("MusicPlayer", "Pipeline state changed from " +
                std::string(gst_element_state_get_name(old_state)) + " to " +
                std::string(gst_element_state_get_name(new_state)));
        }
        break;
    }

    case GST_MESSAGE_STREAM_START: {
        LOG_INFO("MusicPlayer", "Stream started");
        break;
    }

    default:
        // Other message types - no special handling
        break;
    }
}
void MusicPlayer::onEos()
{
    // Called when the end of stream is reached
    if (isShuttingDown_) {
        return;
    }

    if (!loopMode_) {
        // Play the next track
        nextTrack();
    }
    else {
        // For loop mode, the track should automatically restart
        // But sometimes we need to manually restart
        playMusic(currentIndex_);
    }
}

const MusicPlayer::TrackMetadata& MusicPlayer::getCurrentTrackMetadata() const
{
    static TrackMetadata emptyMetadata;

    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_];
    }
    return emptyMetadata;
}

const MusicPlayer::TrackMetadata& MusicPlayer::getTrackMetadata(int index) const
{
    static TrackMetadata emptyMetadata;

    if (index >= 0 && index < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[index];
    }
    return emptyMetadata;
}

size_t MusicPlayer::getTrackMetadataCount() const
{
    return trackMetadata_.size();
}

std::string MusicPlayer::getCurrentTitle() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].title;
    }
    return "";
}

std::string MusicPlayer::getCurrentArtist() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].artist;
    }
    return "";
}

std::string MusicPlayer::getCurrentAlbum() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].album;
    }
    return "";
}

std::string MusicPlayer::getCurrentYear() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].year;
    }
    return "";
}

std::string MusicPlayer::getCurrentGenre() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].genre;
    }
    return "";
}

std::string MusicPlayer::getCurrentComment() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].comment;
    }
    return "";
}

int MusicPlayer::getCurrentTrackNumber() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
        return trackMetadata_[currentIndex_].trackNumber;
    }
    return 0;
}

std::string MusicPlayer::getFormattedTrackInfo(int index) const
{
    if (index == -1) {
        index = currentIndex_;
    }

    if (index < 0 || index >= static_cast<int>(trackMetadata_.size())) {
        return "";
    }

    const auto& meta = trackMetadata_[index];
    std::string info = meta.title;

    if (!meta.artist.empty()) {
        info += " - " + meta.artist;
    }

    if (!meta.album.empty()) {
        info += " (" + meta.album;
        if (!meta.year.empty()) {
            info += ", " + meta.year;
        }
        info += ")";
    }

    return info;
}

std::string MusicPlayer::getTrackArtist(int index) const
{
    if (index == -1) {
        index = currentIndex_;
    }

    if (index < 0 || index >= static_cast<int>(trackMetadata_.size())) {
        return "";
    }

    return trackMetadata_[index].artist;
}

std::string MusicPlayer::getTrackAlbum(int index) const
{
    if (index == -1) {
        index = currentIndex_;
    }

    if (index < 0 || index >= static_cast<int>(trackMetadata_.size())) {
        return "";
    }

    return trackMetadata_[index].album;
}

bool MusicPlayer::playMusic(int index, int customFadeMs)
{
    // Ignore customFadeMs as we're removing fading logic

    // Validate index
    if (index == -1)
    {
        // Use current or choose default as in your original code
        if (currentIndex_ >= 0)
        {
            index = currentIndex_;
        }
        else if (shuffleMode_ && !musicFiles_.empty())
        {
            if (shuffledIndices_.empty())
            {
                setShuffle(true);
            }
            index = shuffledIndices_[currentShufflePos_];
        }
        else if (!musicFiles_.empty())
        {
            index = 0;
        }
        else
        {
            LOG_WARNING("MusicPlayer", "No music tracks available to play");
            return false;
        }
    }

    // Check that the index is valid.
    if (index < 0 || index >= static_cast<int>(musicFiles_.size()))
    {
        LOG_ERROR("MusicPlayer", "Invalid track index: " + std::to_string(index));
        return false;
    }

    // Stop any currently playing music
    if (isPlaying() || isPaused()) {
        stopMusic();
    }

    // Load and play the new track
    loadTrack(index);

    // If shuffle mode is enabled, update the current shuffle position
    if (shuffleMode_)
    {
        auto it = std::find(shuffledIndices_.begin(), shuffledIndices_.end(), index);
        if (it != shuffledIndices_.end())
        {
            currentShufflePos_ = static_cast<int>(std::distance(shuffledIndices_.begin(), it));
        }
        else
        {
            // If for some reason the track isn't in the current shuffle order, regenerate it.
            setShuffle(true);
        }
    }

    // Start playback with error checking
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("MusicPlayer", "Failed to play music: " + std::string(musicNames_[index]));

        // Try to get more information about why it failed
        GstMessage* msg = gst_bus_pop_filtered(gst_pipeline_get_bus(GST_PIPELINE(pipeline_)),
            GST_MESSAGE_ERROR);
        if (msg) {
            handleMessage(msg);
            gst_message_unref(msg);
        }

        return false;
    }
    else if (ret == GST_STATE_CHANGE_ASYNC) {
        LOG_INFO("MusicPlayer", "Playback starting asynchronously");
    }

    LOG_INFO("MusicPlayer", "Now playing track: " + getFormattedTrackInfo(index));
    return true;
}

double MusicPlayer::saveCurrentMusicPosition()
{
    if (pipeline_) {
        gint64 position = 0;
        if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &position)) {
            return position / (double)GST_SECOND;
        }
    }
    return 0.0;
}

bool MusicPlayer::pauseMusic(int customFadeMs)
{
    // Ignore customFadeMs as we're removing fading logic

    if (!isPlaying()) {
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("MusicPlayer", "Failed to pause music");
        return false;
    }

    LOG_INFO("MusicPlayer", "Music paused");
    return true;
}

bool MusicPlayer::resumeMusic(int customFadeMs)
{
    // Ignore customFadeMs as we're removing fading logic

    if (!isPaused()) {
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("MusicPlayer", "Failed to resume music");
        return false;
    }

    LOG_INFO("MusicPlayer", "Music resumed");
    return true;
}

bool MusicPlayer::stopMusic(int customFadeMs)
{
    // Ignore customFadeMs as we're removing fading logic

    if (!isPlaying() && !isPaused()) {
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("MusicPlayer", "Failed to stop music");
        return false;
    }

    LOG_INFO("MusicPlayer", "Music stopped");
    return true;
}

bool MusicPlayer::nextTrack(int customFadeMs)
{
    // Ignore customFadeMs as we're removing fading logic

    if (musicFiles_.empty()) {
        return false;
    }

    trackChangeDirection_ = TrackChangeDirection::NEXT;

    int nextIndex;

    if (shuffleMode_)
    {
        // In shuffle mode, move to the next track in the shuffled order
        currentShufflePos_ = (currentShufflePos_ + 1) % shuffledIndices_.size();
        nextIndex = shuffledIndices_[currentShufflePos_];
    }
    else
    {
        // In sequential mode, move to the next track in the list
        nextIndex = (currentIndex_ + 1) % musicFiles_.size();
    }

    return playMusic(nextIndex);
}

int MusicPlayer::getNextTrackIndex()
{
    if (shuffleMode_)
    {
        // In shuffle mode, step forward in the shuffled order.
        if (shuffledIndices_.empty())
            return -1; // Safety check

        int nextPos = (currentShufflePos_ + 1) % shuffledIndices_.size();
        return shuffledIndices_[nextPos];
    }
    else
    {
        // Sequential playback when shuffle is off.
        return (currentIndex_ + 1) % musicFiles_.size();
    }
}

bool MusicPlayer::previousTrack(int customFadeMs)
{
    // Ignore customFadeMs as we're removing fading logic

    if (musicFiles_.empty()) {
        return false;
    }

    trackChangeDirection_ = TrackChangeDirection::PREVIOUS;

    int prevIndex;

    if (shuffleMode_)
    {
        // In shuffle mode, move to the previous track in the shuffled order
        currentShufflePos_ = (currentShufflePos_ - 1 + static_cast<int>(shuffledIndices_.size())) % static_cast<int>(shuffledIndices_.size());
        prevIndex = shuffledIndices_[currentShufflePos_];
    }
    else
    {
        // In sequential mode, move to the previous track in the list
        prevIndex = (currentIndex_ - 1 + static_cast<int>(musicFiles_.size())) % static_cast<int>(musicFiles_.size());
    }

    return playMusic(prevIndex);
}

bool MusicPlayer::isPlaying() const
{
    GstState state;
    gst_element_get_state(pipeline_, &state, NULL, 0);
    return state == GST_STATE_PLAYING;
}

bool MusicPlayer::isPaused() const
{
    GstState state;
    gst_element_get_state(pipeline_, &state, NULL, 0);
    return state == GST_STATE_PAUSED;
}

void MusicPlayer::setVolume(int newVolume)
{
    // Ensure volume is within range (0-100)
    volumeLevel_ = std::max(0, std::min(100, newVolume));

    // Convert from 0-100 range to 0.0-1.0 for GStreamer
    double gstVolume = volumeLevel_ / 100.0;
    g_object_set(source_, "volume", gstVolume, NULL);

    // Save to config if available
    if (config_)
    {
        // No conversion needed since we're using 0-100 directly
        config_->setProperty("musicPlayer.volume", volumeLevel_);
    }

    LOG_INFO("MusicPlayer", "Volume set to " + std::to_string(volumeLevel_) + "%");
}

int MusicPlayer::getVolume() const
{
    return volumeLevel_; // Return cached volume value
}

std::string MusicPlayer::getCurrentTrackName() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicNames_.size()))
    {
        return musicNames_[currentIndex_];
    }
    return "";
}

std::string MusicPlayer::getCurrentTrackNameWithoutExtension() const
{
    // First get the full filename with extension
    std::string fullName;
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicNames_.size()))
    {
        fullName = musicNames_[currentIndex_];
    }
    else
    {
        return "";
    }

    // Find the last occurrence of a dot to identify the extension
    size_t lastDotPos = fullName.find_last_of('.');

    // If no dot is found, return the full filename
    if (lastDotPos == std::string::npos)
    {
        return fullName;
    }

    // Return everything before the last dot
    return fullName.substr(0, lastDotPos);
}

std::string MusicPlayer::getCurrentTrackPath() const
{
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicFiles_.size()))
    {
        return musicFiles_[currentIndex_];
    }
    return "";
}

int MusicPlayer::getCurrentTrackIndex() const
{
    return currentIndex_;
}

int MusicPlayer::getTrackCount() const
{
    return static_cast<int>(musicFiles_.size());
}

void MusicPlayer::setLoop(bool loop)
{
    loopMode_ = loop;

    // Save to config if available
    if (config_)
    {
        config_->setProperty("musicPlayer.loop", loopMode_);
    }

    LOG_INFO("MusicPlayer", "Loop mode " + std::string(loopMode_ ? "enabled" : "disabled"));
}

bool MusicPlayer::getLoop() const
{
    return loopMode_;
}

bool MusicPlayer::shuffle()
{
    if (musicFiles_.empty())
    {
        return false;
    }

    // Get a random track and play it
    std::uniform_int_distribution<size_t> dist(0, musicFiles_.size() - 1);
    auto randomIndex = static_cast<int>(dist(rng_));
    return playMusic(randomIndex);
}

bool MusicPlayer::setShuffle(bool shuffle)
{
    shuffleMode_ = shuffle;

    if (shuffleMode_)
    {
        // Build a shuffled order for all tracks.
        shuffledIndices_.clear();
        for (int i = 0; i < static_cast<int>(musicFiles_.size()); i++) {
            shuffledIndices_.push_back(i);
        }
        std::shuffle(shuffledIndices_.begin(), shuffledIndices_.end(), rng_);

        // If a track is currently playing, update currentShufflePos to its position in the shuffled list.
        if (currentIndex_ >= 0)
        {
            auto it = std::find(shuffledIndices_.begin(), shuffledIndices_.end(), currentIndex_);
            if (it != shuffledIndices_.end())
                currentShufflePos_ = static_cast<int>(std::distance(shuffledIndices_.begin(), it));
            else
                currentShufflePos_ = 0;
        }
        else
        {
            currentShufflePos_ = 0;
        }
    }
    else
    {
        // When shuffle is off, clear the shuffle order.
        shuffledIndices_.clear();
        currentShufflePos_ = -1;
    }

    // Save to config if available.
    if (config_)
    {
        config_->setProperty("musicPlayer.shuffle", shuffleMode_);
    }

    LOG_INFO("MusicPlayer", "Shuffle mode " + std::string(shuffleMode_ ? "enabled" : "disabled"));
    return true;
}

bool MusicPlayer::getShuffle() const
{
    return shuffleMode_;
}

bool MusicPlayer::hasTrackChanged()
{
    std::string currentTrackPath = getCurrentTrackPath();
    bool changed = !currentTrackPath.empty() && (currentTrackPath != lastCheckedTrackPath_);

    // Update last checked track
    if (changed) {
        lastCheckedTrackPath_ = currentTrackPath;
    }

    return changed;
}

bool MusicPlayer::isPlayingNewTrack()
{
    // Only report change if music is actually playing
    return isPlaying() && hasTrackChanged();
}

bool MusicPlayer::getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData)
{
    return false;
}

double MusicPlayer::getCurrent()
{
    if (!isPlaying() && !isPaused()) {
        return -1.0;
    }

    gint64 position = 0;
    if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &position)) {
        return position / (double)GST_SECOND;
    }

    return -1.0;
}

double MusicPlayer::getDuration()
{
    if (!isPlaying() && !isPaused()) {
        return -1.0;
    }

    gint64 duration = 0;
    if (gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
        return duration / (double)GST_SECOND;
    }

    return -1.0;
}

void MusicPlayer::setTrackChangeDirection(TrackChangeDirection direction)
{
    trackChangeDirection_ = direction;
}

MusicPlayer::TrackChangeDirection MusicPlayer::getTrackChangeDirection() const
{
    return trackChangeDirection_;
}

bool MusicPlayer::isFading() const
{
    // Without custom fading, we just return false
    return false;
}

void MusicPlayer::shutdown()
{
    LOG_INFO("MusicPlayer", "Shutting down music player");

    // Set flag first to prevent callbacks
    isShuttingDown_ = true;

    // Stop any playing music
    if (isPlaying() || isPaused()) {
        stopMusic();
    }

    // Clear playlists
    musicFiles_.clear();
    musicNames_.clear();

    currentIndex_ = -1;
    LOG_INFO("MusicPlayer", "Music player shutdown complete");
}