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
#include <SDL2/SDL_image.h>
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
	, pausedMusicPosition(0.0)
	, isPendingTrackChange(false)
	, pendingTrackIndex(-1)
	, fadeMs(1500)
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

	int configFadeMs;
	if (config.getProperty("musicPlayer.fadeMs", configFadeMs))
	{
		fadeMs = std::max(0, configFadeMs);
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
		", fade: " + std::to_string(fadeMs) + "ms" +
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

bool MusicPlayer::isValidAudioFile(const std::string& filePath) const
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

bool MusicPlayer::readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const
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

bool MusicPlayer::playMusic(int index, int customFadeMs)
{
	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs : customFadeMs;

	// Validate index
	if (index == -1)
	{
		// Use current or choose default as in your original code
		if (currentIndex >= 0)
		{
			index = currentIndex;
		}
		else if (shuffleMode && !musicFiles.empty())
		{
			if (shuffledIndices.empty())
			{
				setShuffle(true);
			}
			index = shuffledIndices[currentShufflePos];
		}
		else if (!musicFiles.empty())
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
	if (index < 0 || index >= static_cast<int>(musicFiles.size()))
	{
		LOG_ERROR("MusicPlayer", "Invalid track index: " + std::to_string(index));
		return false;
	}

	// Clear any pending pause state
	isPendingPause = false;

	// If music is already playing or fading, fade it out first
	if (Mix_PlayingMusic() || Mix_FadingMusic() != MIX_NO_FADING)
	{
		if (useFadeMs > 0)
		{
			// Set up for pending track change after fade out
			isPendingTrackChange = true;
			pendingTrackIndex = index;

			// Fade out current music
			if (Mix_FadeOutMusic(useFadeMs) == 0)
			{
				LOG_WARNING("MusicPlayer", "Failed to fade out music, stopping immediately");
				Mix_HaltMusic();
			}
			else
			{
				LOG_INFO("MusicPlayer", "Fading out current track before changing to new track");
				return true; // Return true, the actual track change will happen in the callback
			}
		}
		else
		{
			// No fade, stop immediately
			Mix_HaltMusic();
		}
	}

	// No fade or music was halted immediately, so load and play the new track
	loadTrack(index);

	if (!currentMusic)
	{
		isPendingTrackChange = false;
		return false;
	}

	// If shuffle mode is enabled, update the current shuffle position
	if (shuffleMode)
	{
		auto it = std::find(shuffledIndices.begin(), shuffledIndices.end(), index);
		if (it != shuffledIndices.end())
		{
			currentShufflePos = static_cast<int>(std::distance(shuffledIndices.begin(), it));
		}
		else
		{
			// If for some reason the track isn't in the current shuffle order, regenerate it.
			setShuffle(true);
		}
	}

	// Play the music with fade-in if specified
	int result;
	if (useFadeMs > 0)
	{
		result = Mix_FadeInMusic(currentMusic, loopMode ? -1 : 1, useFadeMs);
		LOG_INFO("MusicPlayer", "Fading in track: " + musicNames[index] + " over " + std::to_string(useFadeMs) + "ms");
	}
	else
	{
		result = Mix_PlayMusic(currentMusic, loopMode ? -1 : 1);
		LOG_INFO("MusicPlayer", "Playing track: " + musicNames[index]);
	}

	if (result == -1)
	{
		LOG_ERROR("MusicPlayer", "Failed to play music: " + std::string(Mix_GetError()));
		return false;
	}

	LOG_INFO("MusicPlayer", "Now playing track: " + getFormattedTrackInfo(index));
	isPendingTrackChange = false;
	return true;
}
double MusicPlayer::saveCurrentMusicPosition()
{
	if (currentMusic)
	{
		// Get the current position in the music in seconds
		// If your SDL_mixer version doesn't support this, you'll need to track time manually
#if SDL_MIXER_MAJOR_VERSION > 2 || (SDL_MIXER_MAJOR_VERSION == 2 && SDL_MIXER_MINOR_VERSION >= 6)
		return Mix_GetMusicPosition(currentMusic);
#else
// For older SDL_mixer versions, we can't get the position
		return 0.0;
#endif
	}
	return 0.0;
}

bool MusicPlayer::pauseMusic(int customFadeMs)
{
	if (!isPlaying() || isPaused())
	{
		return false;
	}

	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs : customFadeMs;

	// Save current position before pausing (for possible resume with fade)
	pausedMusicPosition = saveCurrentMusicPosition();

	if (useFadeMs > 0)
	{
		// Set flags to indicate this is a pause operation
		isPendingPause = true;
		isPendingTrackChange = false;
		pendingTrackIndex = -1;

		// Fade out and then pause
		if (Mix_FadeOutMusic(useFadeMs) == 0)
		{
			// Failed to fade out, pause immediately
			LOG_WARNING("MusicPlayer", "Failed to fade out before pause, pausing immediately");
			Mix_PauseMusic();
			isPendingPause = false;
		}
		else
		{
			LOG_INFO("MusicPlayer", "Fading out music before pausing over " + std::to_string(useFadeMs) + "ms");
			// The actual pause will be handled in the musicFinishedCallback
		}
	}
	else
	{
		// No fade, pause immediately
		Mix_PauseMusic();
		LOG_INFO("MusicPlayer", "Music paused");
	}

	return true;
}

bool MusicPlayer::resumeMusic(int customFadeMs)
{
	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs : customFadeMs;

	// If we're in a paused state after fade-out, we need to load the track and start it
	if (isPendingPause)
	{
		isPendingPause = false;

		// If we have a saved position and the track is still valid
		if (pausedMusicPosition > 0.0 && currentIndex >= 0 && currentIndex < static_cast<int>(musicFiles.size()))
		{
			// Load the track
			loadTrack(currentIndex);

			if (!currentMusic)
			{
				LOG_ERROR("MusicPlayer", "Failed to reload track for resume");
				return false;
			}

			// Calculate the adjusted position - add the fade duration in seconds
			// This ensures we don't repeat music that was playing during the fade-out
			double adjustedPosition = pausedMusicPosition;

			// Only add the fade time if it was a non-zero fade and if we're not at the beginning
			if (fadeMs > 0 && pausedMusicPosition > 0.0)
			{
				// Convert fadeMs from milliseconds to seconds and add
				adjustedPosition += fadeMs / 1000.0;

				// Get the music length if possible to avoid going past the end
#if SDL_MIXER_MAJOR_VERSION > 2 || (SDL_MIXER_MAJOR_VERSION == 2 && SDL_MIXER_MINOR_VERSION >= 6)
				double musicLength = Mix_MusicDuration(currentMusic);
				// If we have a valid duration and our adjusted position exceeds it
				if (musicLength > 0 && adjustedPosition >= musicLength)
				{
					// If looping is on, wrap around
					if (loopMode)
					{
						adjustedPosition = std::fmod(adjustedPosition, musicLength);
					}
					// Otherwise cap at just before the end
					else
					{
						LOG_INFO("MusicPlayer", "Adjusted position would exceed track length, playing next track instead");
						return nextTrack(useFadeMs);
					}
				}
#endif
			}

			// Start playback from the adjusted position with fade-in
#if SDL_MIXER_MAJOR_VERSION > 2 || (SDL_MIXER_MAJOR_VERSION == 2 && SDL_MIXER_MINOR_VERSION >= 6)
			if (Mix_FadeInMusicPos(currentMusic, loopMode ? -1 : 1, useFadeMs, adjustedPosition) == -1)
			{
				LOG_ERROR("MusicPlayer", "Failed to resume music with fade: " + std::string(Mix_GetError()));
				return false;
			}
#else
			if (Mix_FadeInMusic(currentMusic, loopMode ? -1 : 1, useFadeMs) == -1)
			{
				LOG_ERROR("MusicPlayer", "Failed to resume music with fade: " + std::string(Mix_GetError()));
				return false;
			}
#endif

			LOG_INFO("MusicPlayer", "Resuming track: " + musicNames[currentIndex] + " from adjusted position " +
				std::to_string(adjustedPosition) + " (original: " + std::to_string(pausedMusicPosition) +
				") with " + std::to_string(useFadeMs) + "ms fade");
			return true;
		}
		else if (currentIndex >= 0 && currentIndex < static_cast<int>(musicFiles.size()))
		{
			// Just restart the track from the beginning
			return playMusic(currentIndex, useFadeMs);
		}
		else
		{
			LOG_ERROR("MusicPlayer", "No valid track to resume");
			return false;
		}
	}
	else if (isPaused())
	{
		// Regular pause (not after fade-out), just resume
		Mix_ResumeMusic();
		LOG_INFO("MusicPlayer", "Music resumed");
		return true;
	}

	return false; // Nothing to resume
}

bool MusicPlayer::stopMusic(int customFadeMs)
{
	if (!Mix_PlayingMusic() && !Mix_PausedMusic() && !isPendingPause)
	{
		return false;
	}

	// Clear any pending pause state
	isPendingPause = false;
	isPendingTrackChange = false;
	pendingTrackIndex = -1;

	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs : customFadeMs;

	if (useFadeMs > 0 && !isShuttingDown)
	{
		// Fade out music
		if (Mix_FadeOutMusic(useFadeMs) == 0)
		{
			// Failed to fade out, stop immediately
			LOG_WARNING("MusicPlayer", "Failed to fade out music, stopping immediately");
			Mix_HaltMusic();
		}
		else
		{
			LOG_INFO("MusicPlayer", "Fading out music over " + std::to_string(useFadeMs) + "ms");
		}
	}
	else
	{
		// Stop immediately
		Mix_HaltMusic();
		LOG_INFO("MusicPlayer", "Music stopped immediately");
	}

	// Reset saved position
	pausedMusicPosition = 0.0;

	return true;
}

bool MusicPlayer::nextTrack(int customFadeMs)
{
	if (musicFiles.empty())
	{
		return false;
	}

	int nextIndex;

	if (shuffleMode)
	{
		// In shuffle mode, move to the next track in the shuffled order
		currentShufflePos = (currentShufflePos + 1) % shuffledIndices.size();
		nextIndex = shuffledIndices[currentShufflePos];
	}
	else
	{
		// In sequential mode, move to the next track in the list
		nextIndex = (currentIndex + 1) % musicFiles.size();
	}

	return playMusic(nextIndex, customFadeMs);
}

int MusicPlayer::getNextTrackIndex()
{
	if (shuffleMode)
	{
		// In shuffle mode, step forward in the shuffled order.
		if (shuffledIndices.empty())
			return -1; // Safety check

		if (currentShufflePos < static_cast<int>(shuffledIndices.size()) - 1)
		{
			currentShufflePos++;
		}
		else
		{
			// Option: Loop back to the start (or alternatively, reshuffle).
			currentShufflePos = 0;
		}
		return shuffledIndices[currentShufflePos];
	}
	else
	{
		// Sequential playback when shuffle is off.
		return (currentIndex + 1) % musicFiles.size();
	}
}

bool MusicPlayer::previousTrack(int customFadeMs)
{
	if (musicFiles.empty())
	{
		return false;
	}

	int prevIndex;

	if (shuffleMode)
	{
		// In shuffle mode, move to the previous track in the shuffled order
		currentShufflePos = (currentShufflePos - 1 + static_cast<int>(shuffledIndices.size())) % static_cast<int>(shuffledIndices.size());
		prevIndex = shuffledIndices[currentShufflePos];
	}
	else
	{
		// In sequential mode, move to the previous track in the list
		prevIndex = (currentIndex - 1 + static_cast<int>(musicFiles.size())) % static_cast<int>(musicFiles.size());
	}

	return playMusic(prevIndex, customFadeMs);
}

bool MusicPlayer::isPlaying() const
{
	return Mix_PlayingMusic() == 1 && !Mix_PausedMusic();
}

bool MusicPlayer::isPaused() const
{
	return Mix_PausedMusic() == 1 || isPendingPause;
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
	auto randomIndex = static_cast<int>(dist(rng));
	return playMusic(randomIndex);
}

bool MusicPlayer::setShuffle(bool shuffle)
{
	shuffleMode = shuffle;

	if (shuffleMode)
	{
		// Build a shuffled order for all tracks.
		shuffledIndices.clear();
		for (int i = 0; i < static_cast<int>(musicFiles.size()); i++) {
			shuffledIndices.push_back(i);
		}
		std::shuffle(shuffledIndices.begin(), shuffledIndices.end(), rng);

		// If a track is currently playing, update currentShufflePos to its position in the shuffled list.
		if (currentIndex >= 0)
		{
			auto it = std::find(shuffledIndices.begin(), shuffledIndices.end(), currentIndex);
			if (it != shuffledIndices.end())
				currentShufflePos = static_cast<int>(std::distance(shuffledIndices.begin(), it));
			else
				currentShufflePos = 0;
		}
		else
		{
			currentShufflePos = 0;
		}
	}
	else
	{
		// When shuffle is off, clear the shuffle order.
		shuffledIndices.clear();
		currentShufflePos = -1;
	}

	// Save to config if available.
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
	// Don't proceed if shutting down
	if (isShuttingDown)
	{
		return;
	}

	// Check if this is a pause operation
	if (isPendingPause)
	{
		// This was a fade-to-pause operation
		Mix_PauseMusic();  // Ensure paused state is set
		LOG_INFO("MusicPlayer", "Music paused after fade-out");
		return;  // Don't continue to next track
	}

	// Check if we're waiting to change tracks after a fade
	if (isPendingTrackChange && pendingTrackIndex >= 0)
	{
		int indexToPlay = pendingTrackIndex;
		isPendingTrackChange = false;
		pendingTrackIndex = -1;

		LOG_INFO("MusicPlayer", "Playing next track after fade: " + std::to_string(indexToPlay));
		playMusic(indexToPlay, fadeMs);  // No fade-in needed after a fade-out
		return;
	}

	// Normal track finished playing
	LOG_INFO("MusicPlayer", "Track finished playing: " + getCurrentTrackName());

	if (!loopMode)  // In loop mode SDL_mixer handles looping internally
	{
		// Play the next track
		nextTrack();
	}
}

void MusicPlayer::setFadeDuration(int ms)
{
	fadeMs = std::max(0, ms);

	// Save to config if available
	if (config)
	{
		config->setProperty("musicPlayer.fadeMs", fadeMs);
	}
}

int MusicPlayer::getFadeDuration() const
{
	return fadeMs;
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

bool extractAlbumArtFromFile(const std::string& filePath, std::vector<unsigned char>& albumArtData) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		SDL_Log("Failed to open file: %s", filePath.c_str());
		return false;
	}

	// Read the ID3v2 header (10 bytes)
	char header[10];
	file.read(header, 10);
	if (file.gcount() < 10 || std::strncmp(header, "ID3", 3) != 0) {
		// Not an ID3v2 file
		return false;
	}

	// Get the tag size (bytes 6-9 are synchsafe integers)
	unsigned char sizeBytes[4];
	std::memcpy(sizeBytes, header + 6, 4);
	int tagSize = 0;
	for (int i = 0; i < 4; ++i) {
		tagSize = (tagSize << 7) | (sizeBytes[i] & 0x7F);
	}
	int tagEnd = 10 + tagSize; // End position of the tag

	// Loop through frames until we reach the end of the tag.
	while (file.tellg() < tagEnd) {
		char frameHeader[10];
		file.read(frameHeader, 10);
		if (file.gcount() < 10)
			break;

		// Frame ID is in the first 4 bytes.
		char frameID[5] = { frameHeader[0], frameHeader[1], frameHeader[2], frameHeader[3], 0 };
		// Get frame size (assuming ID3v2.3 - big-endian integer)
		int frameSize = (static_cast<unsigned char>(frameHeader[4]) << 24) |
			(static_cast<unsigned char>(frameHeader[5]) << 16) |
			(static_cast<unsigned char>(frameHeader[6]) << 8) |
			(static_cast<unsigned char>(frameHeader[7]));

		if (frameSize <= 0)
			break;

		if (std::strcmp(frameID, "APIC") == 0) {
			// Read the entire frame data.
			std::vector<unsigned char> frameData(frameSize);
			file.read(reinterpret_cast<char*>(frameData.data()), frameSize);
			if (static_cast<int>(frameData.size()) < frameSize)
				break;

			size_t offset = 0;

			// Skip text encoding (1 byte)
			offset += 1;

			// Skip MIME type (null-terminated string)
			while (offset < frameData.size() && frameData[offset] != 0)
				offset++;
			offset++; // Skip the null terminator

			// The next byte is the picture type.
			if (offset >= frameData.size())
				break;
			unsigned char pictureType = frameData[offset];
			offset++; // Move past picture type

			// We only want the front cover (picture type == 3)
			if (pictureType != 0x03) {
				// Not the front cover; skip this frame.
				continue;
			}

			// Skip description (null-terminated string)
			while (offset < frameData.size() && frameData[offset] != 0)
				offset++;
			offset++; // Skip the null terminator

			if (offset < frameData.size()) {
				// The rest of the frame is the image data.
				albumArtData.assign(frameData.begin() + offset, frameData.end());
				return true;
			}
			else {
				return false;
			}
		}
		else {
			// Skip this frame's data if not APIC.
			file.seekg(frameSize, std::ios::cur);
		}
	}
	return false;
}

SDL_Texture* MusicPlayer::getAlbumArt(SDL_Renderer* renderer, int trackIndex) {
	// Validate track index.
	if (trackIndex < 0 || trackIndex >= static_cast<int>(musicFiles.size())) {
		return nullptr;
	}

	// Get the file path of the requested track.
	std::string filePath = musicFiles[trackIndex];

	// Extract album art data from the file.
	std::vector<unsigned char> albumArtData;
	if (!extractAlbumArtFromFile(filePath, albumArtData) || albumArtData.empty()) {
		// No album art available.
		return nullptr;
	}

	// Create an SDL_RWops from the album art data.
	SDL_RWops* rw = SDL_RWFromConstMem(albumArtData.data(), static_cast<int>(albumArtData.size()));
	if (!rw) {
		SDL_Log("Failed to create RWops: %s", SDL_GetError());
		return nullptr;
	}

	// Load the image into an SDL_Surface using SDL_image.
	SDL_Surface* imageSurface = IMG_Load_RW(rw, 1); // The '1' indicates SDL will close the RWops.
	if (!imageSurface) {
		SDL_Log("Failed to load image from RWops: %s", IMG_GetError());
		return nullptr;
	}

	// Create an SDL_Texture from the surface.
	SDL_Texture* albumArtTexture = SDL_CreateTextureFromSurface(renderer, imageSurface);
	if (!albumArtTexture) {
		SDL_Log("Failed to create texture: %s", SDL_GetError());
	}

	SDL_FreeSurface(imageSurface);

	return albumArtTexture;
}