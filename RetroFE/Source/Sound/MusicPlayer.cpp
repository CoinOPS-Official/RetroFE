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
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

MusicPlayer* MusicPlayer::instance = nullptr;

MusicPlayer* MusicPlayer::getInstance()
{
	if (!instance)
	{
		instance = new MusicPlayer();
	}
	return instance;
}

MusicPlayer::MusicPlayer()
	: config(nullptr)
	, currentMusic(nullptr)
	, currentIndex(-1)
	, volume(MIX_MAX_VOLUME)
	, loopMode(false)
	, shuffleMode(false)
	, isShuttingDown(false)

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

	rng.seed(seq);
}

MusicPlayer::~MusicPlayer()
{
	isShuttingDown = true;
	stopMusic();
	if (currentMusic)
	{
		Mix_FreeMusic(currentMusic);
		currentMusic = nullptr;
	}
}

bool MusicPlayer::initialize(Configuration& config)
{
	this->config = &config;

	// Get volume from config if available
	int configVolume;
	if (config.getProperty("musicPlayer.volume", configVolume))
	{
		volume = std::max(0, std::min(MIX_MAX_VOLUME, configVolume));
	}

	// Set the music callback for handling when music finishes
	Mix_HookMusicFinished(MusicPlayer::musicFinishedCallback);

	// Set music volume
	Mix_VolumeMusic(volume);

	// Get loop mode from config
	bool configLoop;
	if (config.getProperty("musicPlayer.loop", configLoop))
	{
		loopMode = configLoop;
	}

	// Get shuffle mode from config
	bool configShuffle;
	if (config.getProperty("musicPlayer.shuffle", configShuffle))
	{
		shuffleMode = configShuffle;
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

	LOG_INFO("MusicPlayer", "Initialized with volume: " + std::to_string(volume) +
		", loop: " + std::to_string(loopMode) +
		", shuffle: " + std::to_string(shuffleMode) +
		", tracks found: " + std::to_string(musicFiles.size()));

	return true;
}

// Helper method to extract the folder loading logic
void MusicPlayer::loadMusicFolderFromConfig()
{
	std::string musicFolder;
	if (config && config->getProperty("musicPlayer.folder", musicFolder))
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
	musicFiles.clear();
	musicNames.clear();
	trackMetadata.clear();

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
			musicFiles.push_back(std::get<0>(entry));
			musicNames.push_back(std::get<1>(entry));
			trackMetadata.push_back(std::get<2>(entry));
		}

		LOG_INFO("MusicPlayer", "Found " + std::to_string(musicFiles.size()) + " music files");
	}
	catch (const std::exception& e)
	{
		LOG_ERROR("MusicPlayer", "Error scanning music directory: " + std::string(e.what()));
		return false;
	}

	return !musicFiles.empty();
}

bool MusicPlayer::loadM3UPlaylist(const std::string& playlistPath)
{
	// Clear existing music files
	musicFiles.clear();
	musicNames.clear();
	trackMetadata.clear();

	LOG_INFO("MusicPlayer", "Loading music from M3U playlist: " + playlistPath);

	if (!parseM3UFile(playlistPath))
	{
		LOG_ERROR("MusicPlayer", "Failed to parse M3U playlist: " + playlistPath);
		return false;
	}

	LOG_INFO("MusicPlayer", "Found " + std::to_string(musicFiles.size()) + " music files in playlist");
	return !musicFiles.empty();
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
			musicFiles.push_back(std::get<0>(entry));
			musicNames.push_back(std::get<1>(entry));
			trackMetadata.push_back(std::get<2>(entry));
		}

		return true;
	}
	catch (const std::exception& e)
	{
		LOG_ERROR("MusicPlayer", "Error parsing M3U playlist: " + std::string(e.what()));
		return false;
	}
}

bool MusicPlayer::isValidAudioFile(const std::string& filePath)
{
	std::string ext = fs::path(filePath).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	return (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".mod");
}

void MusicPlayer::loadTrack(int index)
{
	// Free any currently playing music
	if (currentMusic)
	{
		Mix_FreeMusic(currentMusic);
		currentMusic = nullptr;
	}

	if (index < 0 || index >= static_cast<int>(musicFiles.size()))
	{
		LOG_ERROR("MusicPlayer", "Invalid track index: " + std::to_string(index));
		currentIndex = -1;
		return;
	}

	// Load the specified track
	currentMusic = Mix_LoadMUS(musicFiles[index].c_str());
	if (!currentMusic)
	{
		LOG_ERROR("MusicPlayer", "Failed to load music file: " + musicFiles[index] + ", Error: " + Mix_GetError());
		currentIndex = -1;
		return;
	}

	currentIndex = index;
	LOG_INFO("MusicPlayer", "Loaded track: " + musicNames[index]);
}

bool MusicPlayer::readTrackMetadata(const std::string& filePath, TrackMetadata& metadata)
{
	// Default to filename without extension as title
	std::string fileName = fs::path(filePath).filename().string();
	size_t lastDot = fileName.find_last_of('.');
	if (lastDot != std::string::npos) {
		metadata.title = fileName.substr(0, lastDot);
	}
	else {
		metadata.title = fileName;
	}

	bool metadataFound = false;

	// Use SDL_mixer to get metadata when available
	Mix_Music* music = Mix_LoadMUS(filePath.c_str());
	if (music) {
		// Get basic metadata
		const char* title = Mix_GetMusicTitle(music);
		const char* artist = Mix_GetMusicArtistTag(music);
		const char* album = Mix_GetMusicAlbumTag(music);

		if (title && strlen(title) > 0) {
			metadata.title = title;
			metadataFound = true;
		}

		if (artist && strlen(artist) > 0) {
			metadata.artist = artist;
			metadataFound = true;
		}

		if (album && strlen(album) > 0) {
			metadata.album = album;
			metadataFound = true;
		}

		// Try to get additional tag information
		// Note: Not all of these functions may be available depending on your SDL_mixer version
		// Add conditionals if needed
#if SDL_MIXER_MAJOR_VERSION > 2 || (SDL_MIXER_MAJOR_VERSION == 2 && SDL_MIXER_MINOR_VERSION >= 6)
		// SDL_mixer 2.6.0 or newer has more tag functions
		const char* copyright = Mix_GetMusicCopyrightTag(music);
		if (copyright && strlen(copyright) > 0) {
			metadata.comment = copyright;
			metadataFound = true;
		}
#endif

		Mix_FreeMusic(music);
	}

	// If we didn't find any metadata, try to parse the filename for artist - title format
	if (!metadataFound && metadata.artist.empty()) {
		// Check for common patterns like "Artist - Title" or "Artist_-_Title"
		std::string name = metadata.title;
		size_t dashPos = name.find(" - ");
		if (dashPos != std::string::npos) {
			metadata.artist = name.substr(0, dashPos);
			metadata.title = name.substr(dashPos + 3);
		}
		else if ((dashPos = name.find("_-_")) != std::string::npos) {
			metadata.artist = name.substr(0, dashPos);
			std::replace(metadata.artist.begin(), metadata.artist.end(), '_', ' ');
			metadata.title = name.substr(dashPos + 3);
			std::replace(metadata.title.begin(), metadata.title.end(), '_', ' ');
		}
	}

	return true;
}

const MusicPlayer::TrackMetadata& MusicPlayer::getCurrentTrackMetadata() const
{
	static TrackMetadata emptyMetadata;

	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex];
	}
	return emptyMetadata;
}

const MusicPlayer::TrackMetadata& MusicPlayer::getTrackMetadata(int index) const
{
	static TrackMetadata emptyMetadata;

	if (index >= 0 && index < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[index];
	}
	return emptyMetadata;
}

size_t MusicPlayer::getTrackMetadataCount() const
{
	return trackMetadata.size();
}

std::string MusicPlayer::getCurrentTitle() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].title;
	}
	return "";
}

std::string MusicPlayer::getCurrentArtist() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].artist;
	}
	return "";
}

std::string MusicPlayer::getCurrentAlbum() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].album;
	}
	return "";
}

std::string MusicPlayer::getCurrentYear() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].year;
	}
	return "";
}

std::string MusicPlayer::getCurrentGenre() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].genre;
	}
	return "";
}

std::string MusicPlayer::getCurrentComment() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].comment;
	}
	return "";
}

int MusicPlayer::getCurrentTrackNumber() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(trackMetadata.size())) {
		return trackMetadata[currentIndex].trackNumber;
	}
	return 0;
}

std::string MusicPlayer::getFormattedTrackInfo(int index) const
{
	if (index == -1) {
		index = currentIndex;
	}

	if (index < 0 || index >= static_cast<int>(trackMetadata.size())) {
		return "";
	}

	const auto& meta = trackMetadata[index];
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
		index = currentIndex;
	}

	if (index < 0 || index >= static_cast<int>(trackMetadata.size())) {
		return "";
	}

	return trackMetadata[index].artist;
}

std::string MusicPlayer::getTrackAlbum(int index) const
{
	if (index == -1) {
		index = currentIndex;
	}

	if (index < 0 || index >= static_cast<int>(trackMetadata.size())) {
		return "";
	}

	return trackMetadata[index].album;
}

bool MusicPlayer::playMusic(int index)
{
	// If index is -1, play current track if there is one, or random if shuffle enabled
	if (index == -1)
	{
		if (currentIndex >= 0)
		{
			index = currentIndex;
		}
		else if (shuffleMode && !musicFiles.empty())
		{
			std::uniform_int_distribution<size_t> dist(0, musicFiles.size() - 1);
			index = static_cast<int>(dist(rng));
		}
		else if (!musicFiles.empty())
		{
			index = 0;  // Default to first track
		}
		else
		{
			LOG_WARNING("MusicPlayer", "No music tracks available to play");
			return false;
		}
	}

	// Check if index is valid
	if (index < 0 || index >= static_cast<int>(musicFiles.size()))
	{
		LOG_ERROR("MusicPlayer", "Invalid track index: " + std::to_string(index));
		return false;
	}

	// Stop any currently playing music
	if (Mix_PlayingMusic())
	{
		Mix_HaltMusic();
	}

	// Load the track
	loadTrack(index);

	// If loading failed
	if (!currentMusic)
	{
		return false;
	}

	// Play the music
	if (Mix_PlayMusic(currentMusic, loopMode ? -1 : 1) == -1)
	{
		LOG_ERROR("MusicPlayer", "Failed to play music: " + std::string(Mix_GetError()));
		return false;
	}

	LOG_INFO("MusicPlayer", "Playing track: " + musicNames[index] + " with tag: " + getFormattedTrackInfo(index));
	return true;
}

bool MusicPlayer::pauseMusic()
{
	if (!isPlaying() || isPaused())
	{
		return false;
	}

	Mix_PauseMusic();
	LOG_INFO("MusicPlayer", "Music paused");
	return true;
}

bool MusicPlayer::resumeMusic()
{
	if (!isPaused())
	{
		return false;
	}

	Mix_ResumeMusic();
	LOG_INFO("MusicPlayer", "Music resumed");
	return true;
}

bool MusicPlayer::stopMusic()
{
	if (!Mix_PlayingMusic() && !Mix_PausedMusic())
	{
		return false;
	}

	// Set the shutdown flag before stopping the music to prevent callback chain
	isShuttingDown = true;

	Mix_HaltMusic();
	LOG_INFO("MusicPlayer", "Music stopped");
	return true;
}

bool MusicPlayer::nextTrack()
{
	if (musicFiles.empty())
	{
		return false;
	}

	int nextIndex = getNextTrackIndex();
	return playMusic(nextIndex);
}

int MusicPlayer::getNextTrackIndex()
{
	if (shuffleMode)
	{
		// Choose a random track that's not the current one
		if (musicFiles.size() > 1)
		{
			int nextIndex;
			do
			{
				std::uniform_int_distribution<size_t> dist(0, musicFiles.size() - 1);
				nextIndex = static_cast<int>(dist(rng));
			} while (nextIndex == currentIndex);
			return nextIndex;
		}
		else
		{
			return 0;  // Only one track, so return it
		}
	}
	else
	{
		// Sequential playback
		return (currentIndex + 1) % musicFiles.size();
	}
}

bool MusicPlayer::previousTrack()
{
	if (musicFiles.empty())
	{
		return false;
	}

	if (shuffleMode)
	{
		// For shuffle mode, just pick another random track
		return nextTrack();
	}
	else
	{
		// Go to previous track in sequence
		int prevIndex = (currentIndex - 1 + static_cast<int>(musicFiles.size())) % static_cast<int>(musicFiles.size());
		return playMusic(prevIndex);
	}
}

bool MusicPlayer::isPlaying() const
{
	return Mix_PlayingMusic() == 1 && !Mix_PausedMusic();
}

bool MusicPlayer::isPaused() const
{
	return Mix_PausedMusic() == 1;
}

void MusicPlayer::setVolume(int newVolume)
{
	// Ensure volume is within SDL_Mixer's range (0-128)
	volume = std::max(0, std::min(MIX_MAX_VOLUME, newVolume));
	Mix_VolumeMusic(volume);

	// Save to config if available
	if (config)
	{
		config->setProperty("musicPlayer.volume", volume);
	}

	LOG_INFO("MusicPlayer", "Volume set to " + std::to_string(volume));
}

int MusicPlayer::getVolume() const
{
	return volume;
}

std::string MusicPlayer::getCurrentTrackName() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(musicNames.size()))
	{
		return musicNames[currentIndex];
	}
	return "";
}

std::string MusicPlayer::getCurrentTrackPath() const
{
	if (currentIndex >= 0 && currentIndex < static_cast<int>(musicFiles.size()))
	{
		return musicFiles[currentIndex];
	}
	return "";
}

int MusicPlayer::getCurrentTrackIndex() const
{
	return currentIndex;
}

int MusicPlayer::getTrackCount() const
{
	return static_cast<int>(musicFiles.size());
}

void MusicPlayer::setLoop(bool loop)
{
	loopMode = loop;

	// If music is currently playing, adjust the loop setting
	if (isPlaying() && currentMusic)
	{
		Mix_HaltMusic();
		Mix_PlayMusic(currentMusic, loopMode ? -1 : 1);
	}

	// Save to config if available
	if (config)
	{
		config->setProperty("musicPlayer.loop", loopMode);
	}

	LOG_INFO("MusicPlayer", "Loop mode " + std::string(loopMode ? "enabled" : "disabled"));
}

bool MusicPlayer::getLoop() const
{
	return loopMode;
}

bool MusicPlayer::shuffle()
{
	if (musicFiles.empty())
	{
		return false;
	}

	// Get a random track and play it
	std::uniform_int_distribution<size_t> dist(0, musicFiles.size() - 1);
	int randomIndex = static_cast<int>(dist(rng));
	return playMusic(randomIndex);
}

bool MusicPlayer::setShuffle(bool shuffle)
{
	shuffleMode = shuffle;

	// Save to config if available
	if (config)
	{
		config->setProperty("musicPlayer.shuffle", shuffleMode);
	}

	LOG_INFO("MusicPlayer", "Shuffle mode " + std::string(shuffleMode ? "enabled" : "disabled"));
	return true;
}

bool MusicPlayer::getShuffle() const
{
	return shuffleMode;
}

void MusicPlayer::musicFinishedCallback()
{
	// This is a static callback, so we need to get the instance
	if (instance)
	{
		instance->onMusicFinished();
	}
}

void MusicPlayer::onMusicFinished()
{
	LOG_INFO("MusicPlayer", "Track finished: " + getCurrentTrackName() + " with tag: " + getFormattedTrackInfo());

	// Don't proceed to the next track if we're shutting down
	if (isShuttingDown)
	{
		LOG_INFO("MusicPlayer", "Not playing next track - application is shutting down");
		return;
	}

	// In loop mode, SDL_Mixer handles the looping automatically for a single track
	// We only need to handle when a track finishes and we need to go to the next one
	if (!loopMode)
	{
		// Play the next track
		nextTrack();
	}
}

void MusicPlayer::resetShutdownFlag()
{
	isShuttingDown = false;
}

void MusicPlayer::shutdown()
{
	LOG_INFO("MusicPlayer", "Shutting down music player");

	// Set flag first to prevent callbacks
	isShuttingDown = true;

	// Stop any playing music
	if (Mix_PlayingMusic() || Mix_PausedMusic())
	{
		Mix_HaltMusic();
	}

	// Free resources
	if (currentMusic)
	{
		Mix_FreeMusic(currentMusic);
		currentMusic = nullptr;
	}

	// Clear playlists
	musicFiles.clear();
	musicNames.clear();

	currentIndex = -1;
	LOG_INFO("MusicPlayer", "Music player shutdown complete");
}

bool MusicPlayer::hasTrackChanged()
{
	std::string currentTrackPath = getCurrentTrackPath();
	bool changed = !currentTrackPath.empty() && (currentTrackPath != lastCheckedTrackPath);

	// Update last checked track
	if (changed) {
		lastCheckedTrackPath = currentTrackPath;
	}

	return changed;
}

bool MusicPlayer::isPlayingNewTrack()
{
	// Only report change if music is actually playing
	return isPlaying() && hasTrackChanged();
}