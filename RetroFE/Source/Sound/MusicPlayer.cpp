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
#include "../Graphics/Component/MusicPlayerComponent.h"
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <thread>
#include <iomanip>
#include <sstream>


namespace fs = std::filesystem;

MusicPlayer* MusicPlayer::instance_ = nullptr;

MusicPlayer* MusicPlayer::getInstance() {
	if (!instance_)
{
		instance_ = new MusicPlayer();
    }
	return instance_;
}

MusicPlayer::MusicPlayer()
	: config_(nullptr)
	, currentMusic_(nullptr)
	, musicFiles_()               // default empty vector
	, musicNames_()               // default empty vector
	, shuffledIndices_()          // default empty vector
	, currentShufflePos_(-1)
	, currentIndex_(-1)
	, volume_(MIX_MAX_VOLUME)
	, logicalVolume_(volume_)
	, loopMode_(false)
	, shuffleMode_(false)
	, isShuttingDown_(false)
	, fadeSerial_(0)
	, rng_()                    // will be seeded below
	, isPendingPause_(false)
	, pausedMusicPosition_(0.0)
	, isPendingTrackChange_(false)
	, pendingTrackIndex_(-1)
	, fadeMs_(1500)
	, previousVolume_(volume_)
	, buttonPressed_(false)
	, lastCheckedTrackPath_("")
	, hasStartedPlaying_(false)
	, lastVolumeChangeTime_(0)
	, volumeChangeIntervalMs_(0)
	, hasVisualizer_(false)
	, audioLevels_()
	, audioChannels_(2)           // Default to stereo
	, hasVuMeter_(false)
	, sampleSize_(2)              // Default to 16-bit samples
{
    // Seed the random number generator with current time
	uint64_t seed = SDL_GetTicks64();
    std::seed_seq seq{
        static_cast<uint32_t>(seed & 0xFFFFFFFF),
        static_cast<uint32_t>((seed >> 32) & 0xFFFFFFFF)
    };
	rng_.seed(seq);

	audioLevels_.resize(audioChannels_, 0.0f);

}

MusicPlayer::~MusicPlayer() {
	isShuttingDown_ = true;
	if (currentMusic_)
{
		Mix_FreeMusic(currentMusic_);
		currentMusic_ = nullptr;
	}
    stopMusic();
    if (currentMusic)
    {
        Mix_FreeMusic(currentMusic);
        currentMusic = nullptr;
    }
}

bool MusicPlayer::initialize(Configuration& config) {
	this->config_ = &config;

    // Get volume from config if available
    int configVolume;
    if (config.getProperty("musicPlayer.volume", configVolume))
    {
		configVolume = std::max(0, std::min(100, configVolume));
		// Convert from percentage (0-100) to internal volume (0-128)
		volume_ = static_cast<int>((configVolume / 100.0f) * MIX_MAX_VOLUME + 0.5f);
    }

    // Set the music callback for handling when music finishes
    Mix_HookMusicFinished(MusicPlayer::musicFinishedCallback);

    // Set music volume
	Mix_VolumeMusic(volume_);

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

	int configFadeMs;
	if (config.getProperty("musicPlayer.fadeMs", configFadeMs))
    {
		fadeMs_ = std::max(0, configFadeMs);
    }

	// --- New Code: Get user-defined volume delay ---
	int configVolumeDelay;
	if (config.getProperty("musicPlayer.volumeDelay", configVolumeDelay))
	{
		// Clamp to range 0 - 50 milliseconds.
		volumeChangeIntervalMs_ = std::max(0, std::min(50, configVolumeDelay));
	}
	// --------------------------------------------------

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

	LOG_INFO("MusicPlayer", "Initialized with volume: " + std::to_string(volume_) +
		", loop: " + std::to_string(loopMode_) +
		", shuffle: " + std::to_string(shuffleMode_) +
		", fade: " + std::to_string(fadeMs_) + "ms" +
		", tracks found: " + std::to_string(musicFiles_.size()));

    return true;
}



void MusicPlayer::addVisualizerListener(MusicPlayerComponent* listener) {
	std::lock_guard<std::mutex> lock(visualizerMutex_);

	// Get format info from currently open audio device
	int frequency;
	Uint16 format;
	int channels;
	if (Mix_QuerySpec(&frequency, &format, &channels) == 1) {
		audioChannels_ = channels;
		audioSampleRate_ = frequency;

		// Determine sample size based on format
		if (format == AUDIO_U8 || format == AUDIO_S8) {
			sampleSize_ = 1;  // 8-bit samples
		}
		else if (format == AUDIO_U16LSB || format == AUDIO_S16LSB ||
			format == AUDIO_U16MSB || format == AUDIO_S16MSB) {
			sampleSize_ = 2;  // 16-bit samples
		}
		else {
			sampleSize_ = 4;  // Assume 32-bit for other formats
		}

		// Resize audio levels array based on channels
		audioLevels_.resize(audioChannels_, 0.0f);
	}

	// Prevent duplicates
	if (std::find(visualizerListeners_.begin(), visualizerListeners_.end(), listener) != visualizerListeners_.end()) {
		return;
	}

	visualizerListeners_.push_back(listener);
	LOG_INFO("MusicPlayer", "Visualizer listener added. Total listeners: " + std::to_string(visualizerListeners_.size()));

	// If this is the FIRST listener, register the master callback.
	if (!hasActiveVisualizers_) {
		Mix_SetPostMix(MusicPlayer::postMixCallback, this);
		hasActiveVisualizers_ = true;
		LOG_INFO("MusicPlayer", "Master post-mix callback registered.");
	}
}

void MusicPlayer::removeVisualizerListener(MusicPlayerComponent* listener) {
	std::lock_guard<std::mutex> lock(visualizerMutex_);

	visualizerListeners_.erase(
		std::remove(visualizerListeners_.begin(), visualizerListeners_.end(), listener),
		visualizerListeners_.end()
	);
	LOG_INFO("MusicPlayer", "Visualizer listener removed. Total listeners: " + std::to_string(visualizerListeners_.size()));

	// If this was the LAST listener, unregister the master callback to save CPU.
	if (visualizerListeners_.empty() && hasActiveVisualizers_) {
		Mix_SetPostMix(nullptr, nullptr);
		hasActiveVisualizers_ = false;
		LOG_INFO("MusicPlayer", "Master post-mix callback unregistered.");
	}
}


void MusicPlayer::postMixCallback(void* udata, Uint8* stream, int len) {
	// This is a static callback, so we need to get the instance
	if (udata) {
		MusicPlayer* player = static_cast<MusicPlayer*>(udata);
		player->processAudioData(stream, len);
	}
}

void MusicPlayer::processAudioData(Uint8* stream, int len) {
	if (!hasActiveVisualizers_ || !stream || len <= 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(visualizerMutex_);
	if (visualizerListeners_.empty()) {
		return;
	}

	// Broadcast the PCM data to every registered listener.
	for (MusicPlayerComponent* listener : visualizerListeners_) {
		if (listener) {
			listener->onPcmDataReceived(stream, len);
		}
	}

	if (hasVuMeter_) {
		// Reset audio levels
		std::fill(audioLevels_.begin(), audioLevels_.end(), 0.0f);

		// Number of samples per channel
		int samplesPerChannel = len / (sampleSize_ * audioChannels_);
		if (samplesPerChannel <= 0) {
			return;
		}

		// Process each channel
		for (int channel = 0; channel < audioChannels_; ++channel) {
			float sum = 0.0f;

			// Process samples for this channel
			for (int i = 0; i < samplesPerChannel; ++i) {
				// Calculate position in the stream for this sample and channel
				int samplePos = (i * audioChannels_ + channel) * sampleSize_;

				// Make sure we're within bounds
				if (samplePos + sampleSize_ > len) {
					break;
				}

				// Get sample value based on format
				float sampleValue = 0.0f;

				if (sampleSize_ == 1) {
					// 8-bit sample (0-255, center at 128)
					Uint8 val = stream[samplePos];
					sampleValue = (static_cast<float>(val) - 128.0f) / 128.0f;
				}
				else if (sampleSize_ == 2) {
					// 16-bit sample (-32768 to 32767)
					Sint16 val = *reinterpret_cast<Sint16*>(stream + samplePos);
					sampleValue = static_cast<float>(val) / 32768.0f;
				}
				else if (sampleSize_ == 4) {
					// 32-bit sample (float -1.0 to 1.0)
					float val = *reinterpret_cast<float*>(stream + samplePos);
					sampleValue = val;
				}

				// Accumulate absolute value for RMS calculation
				sum += sampleValue * sampleValue;
			}

			// Calculate RMS (Root Mean Square) value for this channel
			float rms = std::sqrt(sum / samplesPerChannel);

			// Store normalized level (0.0 - 1.0)
			audioLevels_[channel] = std::min(1.0f, rms);
		}
	}
}

// Helper method to extract the folder loading logic
void MusicPlayer::loadMusicFolderFromConfig() {
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

bool MusicPlayer::loadMusicFolder(const std::string& folderPath) {
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

bool MusicPlayer::loadM3UPlaylist(const std::string& playlistPath) {
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

bool MusicPlayer::parseM3UFile(const std::string& playlistPath) {
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

bool MusicPlayer::isValidAudioFile(const std::string& filePath) const {
	std::string ext = fs::path(filePath).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	return (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".mod");
}

void MusicPlayer::loadTrack(int index) {
    // Free any currently playing music
	if (currentMusic_)
    {
		Mix_FreeMusic(currentMusic_);
		currentMusic_ = nullptr;
    }

	if (index < 0 || index >= static_cast<int>(musicFiles_.size()))
    {
        LOG_ERROR("MusicPlayer", "Invalid track index: " + std::to_string(index));
		currentIndex_ = -1;
        return;
    }

    // Load the specified track
	currentMusic_ = Mix_LoadMUS(musicFiles_[index].c_str());
	if (!currentMusic_)
    {
		LOG_ERROR("MusicPlayer", "Failed to load music file: " + musicFiles_[index] + ", Error: " + Mix_GetError());
		currentIndex_ = -1;
        return;
    }

	currentIndex_ = index;
	LOG_INFO("MusicPlayer", "Loaded track: " + musicNames_[index]);
}

// Utility: Convert syncsafe integer (for ID3v2.4)
static uint32_t syncsafe_to_int(const uint8_t* buf) {
	return ((buf[0] & 0x7f) << 21) | ((buf[1] & 0x7f) << 14) | ((buf[2] & 0x7f) << 7) | (buf[3] & 0x7f);
}

static std::string read_id3v2_text_frame(const char* data, size_t size) {
	if (size < 2) return "";
	uint8_t encoding = data[0];

	if (encoding == 0 || encoding == 3) {
		// ISO-8859-1 or UTF-8
		std::string value(data + 1, size - 1);
		size_t end = value.find_last_not_of(" \0");
		if (end != std::string::npos)
			value.erase(end + 1);
		else
			value.clear();
		return value;
	}
	else if (encoding == 1 || encoding == 2) {
		// UTF-16 (with or without BOM)
		if (size < 4) return "";
		bool bigEndian = false;
		size_t offset = 1;
		if ((unsigned char)data[1] == 0xFE && (unsigned char)data[2] == 0xFF) {
			bigEndian = true;
			offset = 3;
		}
		else if ((unsigned char)data[1] == 0xFF && (unsigned char)data[2] == 0xFE) {
			bigEndian = false;
			offset = 3;
		}
		size_t len = size - offset;
		if (len % 2 != 0) --len;

		std::wstring wstr;
		for (size_t i = 0; i < len; i += 2) {
			wchar_t ch;
			if (bigEndian)
				ch = (static_cast<unsigned char>(data[offset + i]) << 8) | static_cast<unsigned char>(data[offset + i + 1]);
			else
				ch = (static_cast<unsigned char>(data[offset + i + 1]) << 8) | static_cast<unsigned char>(data[offset + i]);
			wstr.push_back(ch);
		}

		std::string value = Utils::wstringToString(wstr);
		size_t end = value.find_last_not_of(" \0");
		if (end != std::string::npos)
			value.erase(end + 1);
		else
			value.clear();
		return value;
	}
	else {
		return "";
	}
}

bool MusicPlayer::readTrackMetadata(const std::string& filePath, TrackMetadata& metadata) const {
	// 1. Try ID3v2 at the start of the file
	std::ifstream file(filePath, std::ios::binary);
	bool tagFound = false;
	if (file) {
		char header[10];
		file.read(header, 10);
		if (file.gcount() == 10 && std::memcmp(header, "ID3", 3) == 0) {
			int version = header[3];
			uint32_t tag_size = syncsafe_to_int(reinterpret_cast<uint8_t*>(header + 6));
			uint32_t bytesRead = 0;
			while (bytesRead < tag_size) {
				char frame_header[10];
				file.read(frame_header, 10);
				if (file.gcount() != 10) break;
				if (std::all_of(frame_header, frame_header + 4, [](char c) { return c == 0; })) break;
				std::string frame_id(frame_header, 4);
				uint32_t frame_size = (version == 4)
					? syncsafe_to_int(reinterpret_cast<uint8_t*>(frame_header + 4))
					: ((uint8_t)frame_header[4] << 24) | ((uint8_t)frame_header[5] << 16) | ((uint8_t)frame_header[6] << 8) | (uint8_t)frame_header[7];
				if (frame_size == 0 || frame_size > 1024 * 1024) break; // sanity check

				std::vector<char> frame_data(frame_size);
				file.read(frame_data.data(), frame_size);
				if ((size_t)file.gcount() != frame_size) break;

				if (frame_id == "TIT2") metadata.title = read_id3v2_text_frame(frame_data.data(), frame_size);
				if (frame_id == "TPE1") metadata.artist = read_id3v2_text_frame(frame_data.data(), frame_size);
				if (frame_id == "TALB") metadata.album = read_id3v2_text_frame(frame_data.data(), frame_size);
				if (frame_id == "TYER" || frame_id == "TDRC") metadata.year = read_id3v2_text_frame(frame_data.data(), frame_size);
				if (frame_id == "TRCK") metadata.trackNumber = std::atoi(read_id3v2_text_frame(frame_data.data(), frame_size).c_str());
				if (frame_id == "TCON") metadata.genre = read_id3v2_text_frame(frame_data.data(), frame_size);
				if (frame_id == "COMM") metadata.comment = "[comment]";
				bytesRead += 10 + frame_size;
			}
			tagFound = !metadata.title.empty() || !metadata.artist.empty() || !metadata.album.empty();
		}
		file.close();
	}

	// 2. If no ID3v2, try ID3v1 at the end of the file
	if (!tagFound) {
		std::ifstream filev1(filePath, std::ios::binary);
		if (filev1) {
			filev1.seekg(-128, std::ios::end);
			char tag[128] = { 0 };
			filev1.read(tag, 128);
			if (filev1.gcount() == 128 && std::string(tag, 3) == "TAG") {
				metadata.title = Utils::trim(std::string(tag + 3, 30));
				metadata.artist = Utils::trim(std::string(tag + 33, 30));
				metadata.album = Utils::trim(std::string(tag + 63, 30));
				metadata.year = Utils::trim(std::string(tag + 93, 4));
				if (tag[125] == 0) {
					metadata.comment = Utils::trim(std::string(tag + 97, 28));
					metadata.trackNumber = static_cast<unsigned char>(tag[126]);
				}
				else {
					metadata.comment = Utils::trim(std::string(tag + 97, 30));
					metadata.trackNumber = 0;
				}
				metadata.genre = std::to_string(static_cast<unsigned char>(tag[127]));
				tagFound = !metadata.title.empty() || !metadata.artist.empty() || !metadata.album.empty();
			}
			filev1.close();
		}
	}

	// 3. Fallback: filename parsing if still nothing
	if (!tagFound) {
		std::string fileName = std::filesystem::path(filePath).filename().string();
		size_t lastDot = fileName.find_last_of('.');
		metadata.title = (lastDot != std::string::npos) ? fileName.substr(0, lastDot) : fileName;

		// Try to guess artist/title from filename: "Artist - Title"
		size_t dashPos = metadata.title.find(" - ");
		if (dashPos != std::string::npos) {
			metadata.artist = metadata.title.substr(0, dashPos);
			metadata.title = metadata.title.substr(dashPos + 3);
		}
		else if ((dashPos = metadata.title.find("_-_")) != std::string::npos) {
			metadata.artist = metadata.title.substr(0, dashPos);
			std::replace(metadata.artist.begin(), metadata.artist.end(), '_', ' ');
			metadata.title = metadata.title.substr(dashPos + 3);
			std::replace(metadata.title.begin(), metadata.title.end(), '_', ' ');
		}
	}

	return true;
}


const MusicPlayer::TrackMetadata& MusicPlayer::getCurrentTrackMetadata() const {
	static TrackMetadata emptyMetadata;

	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_];
	}
	return emptyMetadata;
}

const MusicPlayer::TrackMetadata& MusicPlayer::getTrackMetadata(int index) const {
	static TrackMetadata emptyMetadata;

	if (index >= 0 && index < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[index];
	}
	return emptyMetadata;
}

size_t MusicPlayer::getTrackMetadataCount() const {
	return trackMetadata_.size();
}

std::string MusicPlayer::getCurrentTitle() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].title;
	}
	return "";
}

std::string MusicPlayer::getCurrentArtist() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].artist;
	}
	return "";
}

std::string MusicPlayer::getCurrentAlbum() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].album;
	}
	return "";
}

std::string MusicPlayer::getCurrentYear() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].year;
	}
	return "";
}

std::string MusicPlayer::getCurrentGenre() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].genre;
	}
	return "";
}

std::string MusicPlayer::getCurrentComment() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].comment;
	}
	return "";
}

int MusicPlayer::getCurrentTrackNumber() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(trackMetadata_.size())) {
		return trackMetadata_[currentIndex_].trackNumber;
	}
	return 0;
}

std::string MusicPlayer::getFormattedTrackInfo(int index) const {
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

	//	if (!meta.album.empty()) {
		//	info += " (" + meta.album;
		//	if (!meta.year.empty()) {
		//		info += ", " + meta.year;
		//	}
		//	info += ")";
		//}

	return info;
}

std::string MusicPlayer::getTrackArtist(int index) const {
	if (index == -1) {
		index = currentIndex_;
	}

	if (index < 0 || index >= static_cast<int>(trackMetadata_.size())) {
		return "";
	}

	return trackMetadata_[index].artist;
}

std::string MusicPlayer::getTrackAlbum(int index) const {
	if (index == -1) {
		index = currentIndex_;
	}

	if (index < 0 || index >= static_cast<int>(trackMetadata_.size())) {
		return "";
	}

	return trackMetadata_[index].album;
}

bool MusicPlayer::playMusic(int index, int customFadeMs) {
	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

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

	// Clear any pending pause state
	isPendingPause_ = false;

	// If music is already playing or fading, fade it out first
	if (Mix_PlayingMusic() || isFading())
	{
		if (useFadeMs > 0)
		{
			// Set up for pending track change after fade out
			isPendingTrackChange_ = true;
			pendingTrackIndex_ = index;

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

	if (!currentMusic_)
    {
		isPendingTrackChange_ = false;
        return false;
    }

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

	// Play the music with fade-in if specified
	int result;
	if (useFadeMs > 0)
	{
		result = Mix_FadeInMusic(currentMusic_, loopMode_ ? -1 : 1, useFadeMs);
		LOG_INFO("MusicPlayer", "Fading in track: " + musicNames_[index] + " over " + std::to_string(useFadeMs) + "ms");
	}
	else
	{
		result = Mix_PlayMusic(currentMusic_, loopMode_ ? -1 : 1);
		LOG_INFO("MusicPlayer", "Playing track: " + musicNames_[index]);
	}

	if (result == -1)
    {
        LOG_ERROR("MusicPlayer", "Failed to play music: " + std::string(Mix_GetError()));
        return false;
    }

	setPlaybackState(PlaybackState::PLAYING);
	LOG_INFO("MusicPlayer", "Now playing track: " + getFormattedTrackInfo(index));
	isPendingTrackChange_ = false;

	if (!hasStartedPlaying_)
	{
		hasStartedPlaying_ = true;
	}

    return true;
}

double MusicPlayer::saveCurrentMusicPosition() {
	if (currentMusic_)
{
		// Get the current position in the music in seconds
		// If your SDL_mixer version doesn't support this, you'll need to track time manually
#if SDL_MIXER_MAJOR_VERSION > 2 || (SDL_MIXER_MAJOR_VERSION == 2 && SDL_MIXER_MINOR_VERSION >= 6)
		return Mix_GetMusicPosition(currentMusic_);
#else
// For older SDL_mixer versions, we can't get the position
		return 0.0;
#endif
	}
	return 0.0;
}

bool MusicPlayer::pauseMusic(int customFadeMs) {
	if (!isPlaying() || isPaused() || isFading())
    {
        return false;
    }

	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

	// Save current position before pausing (for possible resume with fade)
	pausedMusicPosition_ = saveCurrentMusicPosition();

	if (useFadeMs > 0)
	{
		// Set flags to indicate this is a pause operation
		isPendingPause_ = true;
		isPendingTrackChange_ = false;
		pendingTrackIndex_ = -1;

		// Fade out and then pause
		if (Mix_FadeOutMusic(useFadeMs) == 0)
		{
			// Failed to fade out, pause immediately
			LOG_WARNING("MusicPlayer", "Failed to fade out before pause, pausing immediately");
			Mix_PauseMusic();
			isPendingPause_ = false;
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
	setPlaybackState(PlaybackState::PAUSED);
    return true;
}

bool MusicPlayer::resumeMusic(int customFadeMs) {
	if (isFading())
		return false;

	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

	// If we're in a paused state after fade-out, we need to load the track and start it
	if (isPendingPause_)
	{
		isPendingPause_ = false;

		// If we have a saved position and the track is still valid
		if (pausedMusicPosition_ > 0.0 && currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicFiles_.size()))
		{
			// Load the track
			loadTrack(currentIndex_);

			if (!currentMusic_)
			{
				LOG_ERROR("MusicPlayer", "Failed to reload track for resume");
				return false;
			}

			// Calculate the adjusted position - add the fade duration in seconds
			// This ensures we don't repeat music that was playing during the fade-out
			double adjustedPosition = pausedMusicPosition_;

			// Only add the fade time if it was a non-zero fade and if we're not at the beginning
			if (fadeMs_ > 0 && pausedMusicPosition_ > 0.0)
			{
				// Convert fadeMs from milliseconds to seconds and add
				adjustedPosition += useFadeMs / 1000.0;

				// Get the music length if possible to avoid going past the end
#if SDL_MIXER_MAJOR_VERSION > 2 || (SDL_MIXER_MAJOR_VERSION == 2 && SDL_MIXER_MINOR_VERSION >= 6)
				double musicLength = Mix_MusicDuration(currentMusic_);
				// If we have a valid duration and our adjusted position exceeds it
				if (musicLength > 0 && adjustedPosition >= musicLength)
				{
					// If looping is on, wrap around
					if (loopMode_)
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
			if (Mix_FadeInMusicPos(currentMusic_, loopMode_ ? -1 : 1, useFadeMs, adjustedPosition) == -1)
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

			LOG_INFO("MusicPlayer", "Resuming track: " + musicNames_[currentIndex_] + " from adjusted position " +
				std::to_string(adjustedPosition) + " (original: " + std::to_string(pausedMusicPosition_) +
				") with " + std::to_string(useFadeMs) + "ms fade");
			setPlaybackState(PlaybackState::PLAYING);
			return true;
		}
		else if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicFiles_.size()))
		{
			// Just restart the track from the beginning
			return playMusic(currentIndex_, useFadeMs);
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
		setPlaybackState(PlaybackState::PLAYING);
    return true;
}

	return false; // Nothing to resume
}

bool MusicPlayer::stopMusic(int customFadeMs) {
	if (!Mix_PlayingMusic() && !Mix_PausedMusic() && !isPendingPause_)
{
    if (!Mix_PlayingMusic() && !Mix_PausedMusic())
    {
        return false;
    }

	// Clear any pending pause state
	isPendingPause_ = false;
	isPendingTrackChange_ = false;
	pendingTrackIndex_ = -1;

	// Use default fade if -1 is passed
	int useFadeMs = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

	if (useFadeMs > 0 && !isShuttingDown_)
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
	pausedMusicPosition_ = 0.0;

    return true;
}

bool MusicPlayer::nextTrack(int customFadeMs) {
	if (musicFiles_.empty() || isFading())
{
    if (musicFiles.empty())
    {
        return false;
    }

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
	setPlaybackState(PlaybackState::NEXT);
	return playMusic(nextIndex, customFadeMs);
}

int MusicPlayer::getNextTrackIndex() {
	if (shuffleMode_)
        {
		// In shuffle mode, step forward in the shuffled order.
		if (shuffledIndices_.empty())
			return -1; // Safety check

		if (currentShufflePos_ < static_cast<int>(shuffledIndices_.size()) - 1)
            {
			currentShufflePos_++;
        }
        else
        {
			// Option: Loop back to the start (or alternatively, reshuffle).
			currentShufflePos_ = 0;
        }
		return shuffledIndices_[currentShufflePos_];
    }
    else
    {
		// Sequential playback when shuffle is off.
		return (currentIndex_ + 1) % musicFiles_.size();
    }
}

bool MusicPlayer::previousTrack(int customFadeMs) {
	if (musicFiles_.empty() || isFading())
{
    if (musicFiles.empty())
    {
        return false;
    }

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
	setPlaybackState(PlaybackState::PREVIOUS);
	return playMusic(prevIndex, customFadeMs);
}

bool MusicPlayer::isPlaying() const {
    return Mix_PlayingMusic() == 1 && !Mix_PausedMusic();
}

bool MusicPlayer::isPaused() const {
	return Mix_PausedMusic() == 1 || isPendingPause_;
}

void MusicPlayer::changeVolume(bool increase) {
	Uint64 now = SDL_GetTicks64();
	if (now - lastVolumeChangeTime_ < volumeChangeIntervalMs_) {
		// Not enough time has passed since the last change
		return;
	}
	lastVolumeChangeTime_ = now;

	int currentVolume = getLogicalVolume();
	int newVolume;
	if (increase) {
		newVolume = std::min(128, currentVolume + 1);
	}
	else {
		newVolume = std::max(0, currentVolume - 1);
	}

	setLogicalVolume(newVolume);
	setButtonPressed(true); // Trigger volume bar update
}

void MusicPlayer::setVolume(int newVolume) {
	++fadeSerial_; // cancel any ongoing fade
	if (isFading())
		return;

    // Ensure volume is within SDL_Mixer's range (0-128)
	volume_ = std::max(0, std::min(MIX_MAX_VOLUME, newVolume));
	Mix_VolumeMusic(volume_);

    // Save to config if available
	if (config_)
    {
		config_->setProperty("musicPlayer.volume", volume_);
    }

	LOG_INFO("MusicPlayer", "Volume set to " + std::to_string(volume_));
}

void MusicPlayer::setLogicalVolume(int v) {
	logicalVolume_ = std::clamp(v, 0, 128);
	if (logicalVolume_ == 0) {
		Mix_VolumeMusic(0);
		return;
	}
	float normalized = static_cast<float>(logicalVolume_) / 128.0f;
	float dB = normalized * 40.0f - 40.0f;
	float gain = std::pow(10.0f, dB / 20.0f);
	int finalVolume = static_cast<int>(gain * 128.0f + 0.5f);
	Mix_VolumeMusic(finalVolume);
}


int MusicPlayer::getLogicalVolume() {
	return logicalVolume_;
}


int MusicPlayer::getVolume() const {
	return Mix_VolumeMusic(-1);
}

void MusicPlayer::fadeToVolume(int targetPercent)
{
	// Clamp target percentage between 0 and 100.
	targetPercent = std::max(0, std::min(100, targetPercent));
	// Convert percentage to Mix_VolumeMusic range.
	int targetVolume = static_cast<int>((targetPercent / 100.0f) * MIX_MAX_VOLUME + 0.5f);

	// Save the current volume (in the 0-128 range) for later restoration.
	previousVolume_ = getVolume();

	// Determine the number of steps for a smooth fade.
	const int steps = 50;
	int sleepDuration = (fadeMs_ > 0) ? (fadeMs_ / steps) : 0;

	// Launch a detached thread to perform the fade.
	std::thread([this, targetVolume, steps, sleepDuration]() {
		int startVolume = getVolume();
		for (int i = 0; i <= steps; ++i)
		{
			// Linear interpolation between startVolume and targetVolume.
			float t = static_cast<float>(i) / steps;
			int newVolume = static_cast<int>(startVolume + t * (targetVolume - startVolume));
			Mix_VolumeMusic(newVolume);
			if (sleepDuration > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
			}
		}
		}).detach();
}

void MusicPlayer::fadeBackToPreviousVolume()
{
	int targetVolume = previousVolume_;
	const int steps = 50;
	int sleepDuration = (fadeMs_ > 0) ? (fadeMs_ / steps) : 0;

	std::thread([this, targetVolume, steps, sleepDuration]() {
		int startVolume = getVolume();
		for (int i = 0; i <= steps; ++i)
		{
			float t = static_cast<float>(i) / steps;
			int newVolume = static_cast<int>(startVolume + t * (targetVolume - startVolume));
			Mix_VolumeMusic(newVolume);
			if (sleepDuration > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
			}
		}
		}).detach();
}

void MusicPlayer::fadeToVolume(int targetVolume, int customFadeMs) {
	int durationMs = (customFadeMs >= 0) ? customFadeMs : fadeMs_;
	targetVolume = std::max(0, std::min(MIX_MAX_VOLUME, targetVolume));
	previousVolume_ = getVolume();

	const int steps = 50;
	int sleepDuration = (durationMs > 0) ? (durationMs / steps) : 0;
	uint32_t mySerial = ++fadeSerial_; // aborts previous fades

	int startVolume = getVolume();

	std::thread([this, mySerial, startVolume, targetVolume, steps, sleepDuration]() {
		for (int i = 0; i <= steps; ++i)
{
			if (mySerial != fadeSerial_ || isShuttingDown_) return; // abort this fade
			float t = static_cast<float>(i) / steps;
			int newVolume = static_cast<int>(startVolume + t * (targetVolume - startVolume));
			Mix_VolumeMusic(newVolume);
			if (sleepDuration > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
}
		}).detach();
}

void MusicPlayer::fadeBackToPreviousVolume() {
	int targetVolume = previousVolume_;
	const int steps = 50;
	int sleepDuration = (fadeMs_ > 0) ? (fadeMs_ / steps) : 0;

	std::thread([this, targetVolume, steps, sleepDuration]() {
		int startVolume = getVolume();
		for (int i = 0; i <= steps; ++i)
{
			float t = static_cast<float>(i) / steps;
			int newVolume = static_cast<int>(startVolume + t * (targetVolume - startVolume));
			Mix_VolumeMusic(newVolume);
			if (sleepDuration > 0)
    {
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
    }
    return "";
}
		}).detach();
}

std::string MusicPlayer::getCurrentTrackName() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicNames_.size()))
{
		return musicNames_[currentIndex_];
    }
    return "";
}

std::string MusicPlayer::getCurrentTrackNameWithoutExtension() const {
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

std::string MusicPlayer::getCurrentTrackPath() const {
	if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(musicFiles_.size()))
{
		return musicFiles_[currentIndex_];
	}
	return "";
}

int MusicPlayer::getCurrentTrackIndex() const {
	return currentIndex_;
}

int MusicPlayer::getTrackCount() const {
	return static_cast<int>(musicFiles_.size());
}

void MusicPlayer::setLoop(bool loop) {
	loopMode_ = loop;

    // If music is currently playing, adjust the loop setting
	if (isPlaying() && currentMusic_)
    {
        Mix_HaltMusic();
		Mix_PlayMusic(currentMusic_, loopMode_ ? -1 : 1);
    }

    // Save to config if available
	if (config_)
    {
		config_->setProperty("musicPlayer.loop", loopMode_);
    }

	LOG_INFO("MusicPlayer", "Loop mode " + std::string(loopMode_ ? "enabled" : "disabled"));
}

bool MusicPlayer::getLoop() const {
	return loopMode_;
}

bool MusicPlayer::shuffle() {
	if (musicFiles_.empty())
{
    if (musicFiles.empty())
    {
        return false;
    }

    // Get a random track and play it
	std::uniform_int_distribution<size_t> dist(0, musicFiles_.size() - 1);
	auto randomIndex = static_cast<int>(dist(rng_));
    return playMusic(randomIndex);
}

bool MusicPlayer::setShuffle(bool shuffle) {
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

bool MusicPlayer::getShuffle() const {
	return shuffleMode_;
}

void MusicPlayer::musicFinishedCallback() {
    // This is a static callback, so we need to get the instance
	if (instance_)
    {
		instance_->onMusicFinished();
    }
}

void MusicPlayer::onMusicFinished() {
	// Don't proceed if shutting down
	if (isShuttingDown_)
{
		return;
	}

	// Check if this is a pause operation
	if (isPendingPause_)
    {
		// This was a fade-to-pause operation
		Mix_PauseMusic();  // Ensure paused state is set
		LOG_INFO("MusicPlayer", "Music paused after fade-out");
		return;  // Don't continue to next track
	}

	// Check if we're waiting to change tracks after a fade
	if (isPendingTrackChange_ && pendingTrackIndex_ >= 0)
	{
		int indexToPlay = pendingTrackIndex_;
		isPendingTrackChange_ = false;
		pendingTrackIndex_ = -1;

		LOG_INFO("MusicPlayer", "Playing next track after fade: " + std::to_string(indexToPlay));
		playMusic(indexToPlay, fadeMs_);
        return;
    }

	// Normal track finished playing
	LOG_INFO("MusicPlayer", "Track finished playing: " + getCurrentTrackName());

	if (!loopMode_)  // In loop mode SDL_mixer handles looping internally
    {
        // Play the next track
        nextTrack();
    }
}

void MusicPlayer::setFadeDuration(int ms) {
	fadeMs_ = std::max(0, ms);

	// Save to config if available
	if (config_)
{
		config_->setProperty("musicPlayer.fadeMs", fadeMs_);
}
}

int MusicPlayer::getFadeDuration() const {
	return fadeMs_;
}

void MusicPlayer::resetShutdownFlag() {
	isShuttingDown_ = false;
}

void MusicPlayer::shutdown() {
    LOG_INFO("MusicPlayer", "Shutting down music player");

    // Set flag first to prevent callbacks
	isShuttingDown_ = true;

	// If music is playing, fade out synchronously
	if (Mix_PlayingMusic()) {
		int steps = 50;
		int fadeMs = fadeMs_; // or use a custom fade for shutdown
		int startVolume = Mix_VolumeMusic(-1);

		for (int i = 0; i <= steps; ++i) {
			if (!Mix_PlayingMusic()) break; // music stopped early
			float t = static_cast<float>(i) / steps;
			int newVolume = static_cast<int>(startVolume * (1.0f - t));
			Mix_VolumeMusic(newVolume);
			std::this_thread::sleep_for(std::chrono::milliseconds(fadeMs / steps));
		}
	}
        Mix_HaltMusic();


    // Free resources
	if (currentMusic_)
    {
		Mix_FreeMusic(currentMusic_);
		currentMusic_ = nullptr;
    }

    // Clear playlists
	musicFiles_.clear();
	musicNames_.clear();

	currentIndex_ = -1;
    LOG_INFO("MusicPlayer", "Music player shutdown complete");
}

bool MusicPlayer::hasTrackChanged() {
	std::string currentTrackPath = getCurrentTrackPath();
	bool changed = !currentTrackPath.empty() && (currentTrackPath != lastCheckedTrackPath_);

	// Update last checked track
	if (changed) {
		lastCheckedTrackPath_ = currentTrackPath;
	}

	return changed;
}

bool MusicPlayer::isPlayingNewTrack() {
	// Only report change if music is actually playing
	return isPlaying() && hasTrackChanged();
}

static bool extractAlbumArtFromFile(const std::string& filePath, std::vector<unsigned char>& albumArtData) {
	try {
		// Clear the output vector first
		albumArtData.clear();

		std::ifstream file(filePath, std::ios::binary);
		if (!file.is_open()) {
			LOG_ERROR("MusicPlayer", "Failed to open file: " + filePath);
			return false;
		}

		// Get file size for validation
		file.seekg(0, std::ios::end);
		std::streamsize fileSize = file.tellg();
		file.seekg(0, std::ios::beg);

		// Ensure file is large enough for ID3 header
		if (fileSize < 10) {
			LOG_INFO("MusicPlayer", "File too small to contain ID3 tags: " + filePath);
			return false;
		}

		// Read the ID3v2 header (10 bytes)
		std::vector<std::byte> header;
		try {
			header.resize(10);
			file.read(reinterpret_cast<char*>(header.data()), header.size());
		}
		catch (const std::bad_alloc& e) {
			LOG_ERROR("MusicPlayer", "Memory allocation failed for ID3 header: " + std::string(e.what()));
			return false;
		}

		if (file.gcount() < static_cast<std::streamsize>(header.size()) ||
			std::memcmp(header.data(), "ID3", 3) != 0) {
			// Not an ID3v2 file
			return false;
		}

		// Get ID3 version
		unsigned int majorVersion = static_cast<unsigned int>(std::to_integer<unsigned char>(header[3]));
		LOG_INFO("MusicPlayer", "ID3v2." + std::to_string(majorVersion) + " tag found");

		// Get the tag size (bytes 6-9 are synchsafe integers)
		int tagSize = 0;
		for (int i = 0; i < 4; ++i) {
			tagSize = (tagSize << 7) | (std::to_integer<unsigned char>(header[6 + i]) & 0x7F);
		}

		// Sanity check on tag size
		if (tagSize <= 0 || tagSize > 100000000) { // 100MB limit
			LOG_WARNING("MusicPlayer", "Invalid tag size: " + std::to_string(tagSize) + " bytes");
			return false;
		}

		// Make sure tag doesn't claim to be larger than the file
		if (tagSize > fileSize - 10) {
			LOG_WARNING("MusicPlayer", "Tag size exceeds file size: " +
				std::to_string(tagSize) + " > " + std::to_string(static_cast<long long>(fileSize - 10)));
			return false;
		}

		int tagEnd = 10 + tagSize; // End position of the tag
		LOG_INFO("MusicPlayer", "Tag size: " + std::to_string(tagSize) + " bytes");

		// Loop through frames until we reach the end of the tag.
		while (file.tellg() < tagEnd && !file.eof()) {
			// Check if we have enough bytes left for a frame header
			if (tagEnd - file.tellg() < 10) {
				LOG_INFO("MusicPlayer", "Not enough data for frame header");
				break;
			}

			std::vector<std::byte> frameHeader;
			try {
				frameHeader.resize(10);
				file.read(reinterpret_cast<char*>(frameHeader.data()), frameHeader.size());
			}
			catch (const std::bad_alloc& e) {
				LOG_ERROR("MusicPlayer", "Memory allocation failed for frame header: " + std::string(e.what()));
				return false;
			}

			if (file.gcount() < static_cast<std::streamsize>(frameHeader.size()))
				break;

			// Frame ID is in the first 4 bytes.
			char frameID[5] = { 0 };
			for (int i = 0; i < 4; i++) {
				char c = static_cast<char>(std::to_integer<unsigned char>(frameHeader[i]));
				// Valid frame IDs only contain A-Z and 0-9
				if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
					frameID[i] = c;
				}
				else {
					// Invalid frame ID character
					LOG_WARNING("MusicPlayer", "Invalid frame ID character: " +
						std::string(1, c) + " (" + std::to_string(static_cast<int>(c)) + ")");
					frameID[0] = 0; // Mark as invalid
					break;
				}
			}

			// If invalid frame ID, we might have reached padding or corrupt data
			if (frameID[0] == 0) {
				LOG_INFO("MusicPlayer", "Invalid frame ID, skipping remainder of tag");
				break;
			}

			// Get frame size (handle different versions correctly)
			int frameSize;
			if (majorVersion >= 4) {
				// ID3v2.4 uses synchsafe integers
				frameSize = 0;
				for (int i = 0; i < 4; ++i) {
					frameSize = (frameSize << 7) | (std::to_integer<unsigned char>(frameHeader[4 + i]) & 0x7F);
				}
			}
			else {
				// ID3v2.3 uses regular integers
				frameSize = (std::to_integer<unsigned char>(frameHeader[4]) << 24) |
					(std::to_integer<unsigned char>(frameHeader[5]) << 16) |
					(std::to_integer<unsigned char>(frameHeader[6]) << 8) |
					(std::to_integer<unsigned char>(frameHeader[7]));
			}

			// Validate frame size
			if (frameSize <= 0 || frameSize > 10000000) { // 10MB limit per frame
				LOG_WARNING("MusicPlayer", "Invalid frame size: " + std::to_string(frameSize));
				break;
			}

			// Check if frame size exceeds remaining tag data
			if (frameSize > tagEnd - file.tellg()) {
				LOG_WARNING("MusicPlayer", "Frame size exceeds remaining tag data: " +
					std::to_string(frameSize) + " > " + std::to_string(static_cast<long long>(tagEnd - file.tellg())));
				break;
			}

			LOG_INFO("MusicPlayer", "Found frame: " + std::string(frameID) +
				", size: " + std::to_string(frameSize) + " bytes");

			if (std::strcmp(frameID, "APIC") == 0) {
				// Read the entire frame data.
				std::vector<std::byte> frameData;
				try {
					frameData.resize(frameSize);
					file.read(reinterpret_cast<char*>(frameData.data()), frameSize);
				}
				catch (const std::bad_alloc& e) {
					LOG_ERROR("MusicPlayer", "Memory allocation failed for APIC frame data: " + std::string(e.what()));
					return false;
				}

				if (file.gcount() < frameSize)
					break;

				size_t offset = 0;

				// Ensure we don't read past the frame data
				if (offset >= frameData.size()) {
					LOG_WARNING("MusicPlayer", "Premature end of APIC frame data");
					break;
				}

				// Skip text encoding (1 byte)
				int textEncoding = std::to_integer<int>(frameData[offset]);
				offset += 1;
				LOG_INFO("MusicPlayer", "Text encoding: " + std::to_string(textEncoding));

				// Skip MIME type (null-terminated string)
				std::string mimeType;
				while (offset < frameData.size() && std::to_integer<unsigned char>(frameData[offset]) != 0) {
					mimeType += static_cast<char>(std::to_integer<unsigned char>(frameData[offset]));
					offset++;
					// Sanity check on MIME type length
					if (mimeType.length() > 100) {
						LOG_WARNING("MusicPlayer", "MIME type too long, probably corrupt data");
						return false;
					}
				}

				// Check if we reached end of data before null terminator
				if (offset >= frameData.size()) {
					LOG_WARNING("MusicPlayer", "MIME type not null-terminated");
					break;
				}

				offset++; // Skip the null terminator
				LOG_INFO("MusicPlayer", "MIME type: " + mimeType);

				// The next byte is the picture type.
				if (offset >= frameData.size()) {
					LOG_WARNING("MusicPlayer", "Premature end of APIC frame data after MIME type");
					break;
				}

				int pictureType = std::to_integer<unsigned char>(frameData[offset]);
				offset++; // Move past picture type
				LOG_INFO("MusicPlayer", "Picture type: " + std::to_string(pictureType));

				// We want either front cover (3) or any picture if desperate
				if (pictureType != 0x03 && pictureType != 0x00) {
					// Skip if not front cover or other picture
					continue;
				}

				// Skip description (null-terminated string)
				// Handle encoding properly
				if (textEncoding == 0 || textEncoding == 3) { // ISO-8859-1 or UTF-8
					// Set a reasonable limit for description scanning to prevent infinite loops
					size_t scanLimit = std::min(frameData.size() - offset, static_cast<size_t>(1000));
					size_t scanned = 0;

					while (offset < frameData.size() && std::to_integer<unsigned char>(frameData[offset]) != 0) {
						offset++;
						scanned++;
						if (scanned >= scanLimit) {
							LOG_WARNING("MusicPlayer", "Description too long or not null-terminated");
							return false;
						}
					}

					// Ensure we didn't reach end of data before null terminator
					if (offset >= frameData.size()) {
						LOG_WARNING("MusicPlayer", "Description not null-terminated");
						break;
					}

					offset++; // Skip the null terminator
				}
				else { // UTF-16/UTF-16BE with BOM
					// Set a reasonable limit for description scanning
					size_t scanLimit = std::min(frameData.size() - offset, static_cast<size_t>(2000));
					size_t scanned = 0;

					while (offset + 1 < frameData.size() &&
						!(std::to_integer<unsigned char>(frameData[offset]) == 0 &&
							std::to_integer<unsigned char>(frameData[offset + 1]) == 0)) {
						offset += 2;
						scanned += 2;
						if (scanned >= scanLimit) {
							LOG_WARNING("MusicPlayer", "UTF-16 description too long or not null-terminated");
							return false;
						}
					}

					// Ensure we didn't reach end of data before double null terminator
					if (offset + 1 >= frameData.size()) {
						LOG_WARNING("MusicPlayer", "UTF-16 description not properly null-terminated");
						break;
					}

					offset += 2; // Skip the double null terminator
				}

				if (offset < frameData.size()) {
					// Calculate remaining bytes for image data
					size_t imageDataSize = frameData.size() - offset;

					// Check we have enough data for a meaningful image
					if (imageDataSize < 100) { // Arbitrary minimum size for a valid image
						LOG_WARNING("MusicPlayer", "Image data too small: " + std::to_string(imageDataSize) + " bytes");
						return false;
					}

					// Log how much image data we're extracting
					LOG_INFO("MusicPlayer", "Extracting " + std::to_string(imageDataSize) + " bytes of image data");

					try {
						// Convert std::byte to unsigned char for albumArtData
						albumArtData.resize(imageDataSize);
						for (size_t i = 0; i < albumArtData.size(); ++i) {
							albumArtData[i] = std::to_integer<unsigned char>(frameData[offset + i]);
						}
					}
					catch (const std::bad_alloc& e) {
						LOG_ERROR("MusicPlayer", "Memory allocation failed for album art data: " + std::string(e.what()));
						return false;
					}

					// Validate the image data starts with proper headers
					if (albumArtData.size() >= 4) {
						// Check if it's a valid JPG/PNG
						if ((albumArtData[0] == 0xFF && albumArtData[1] == 0xD8) || // JPEG
							(albumArtData[0] == 0x89 && albumArtData[1] == 0x50 && albumArtData[2] == 0x4E && albumArtData[3] == 0x47)) { // PNG
							LOG_INFO("MusicPlayer", "Valid image header detected");
							return true;
						}
						else {
							// Format the hex values using stringstream
							std::stringstream ss;
							ss << std::hex << std::uppercase << std::setfill('0')
								<< std::setw(2) << static_cast<int>(albumArtData[0]) << " "
								<< std::setw(2) << static_cast<int>(albumArtData[1]) << " "
								<< std::setw(2) << static_cast<int>(albumArtData[2]) << " "
								<< std::setw(2) << static_cast<int>(albumArtData[3]);

							LOG_WARNING("MusicPlayer", "Warning: Invalid image header: " + ss.str());
							// Continue anyway, IMG_Load might still handle it
							return true;
						}
					}
					else {
						LOG_WARNING("MusicPlayer", "Image data too small: " + std::to_string(albumArtData.size()) + " bytes");
						return false;
					}
				}
				else {
					LOG_WARNING("MusicPlayer", "Invalid APIC frame structure");
					return false;
				}
			}
			else {
				// Skip this frame's data if not APIC.
				file.seekg(frameSize, std::ios::cur);

				// Check if seek operation succeeded
				if (file.fail()) {
					LOG_WARNING("MusicPlayer", "Failed to seek past frame data");
					break;
				}
			}
		}

		LOG_INFO("MusicPlayer", "No suitable album art found");
		return false;
	}
	catch (const std::exception& e) {
		LOG_ERROR("MusicPlayer", "Exception extracting album art: " + std::string(e.what()));
		return false;
	}
	catch (...) {
		LOG_ERROR("MusicPlayer", "Unknown exception extracting album art");
		return false;
	}
}

bool MusicPlayer::getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData) {
	try {
		// Clear the output vector first
		albumArtData.clear();

		// Validate track index
		if (trackIndex < 0 || trackIndex >= static_cast<int>(musicFiles_.size())) {
			LOG_ERROR("MusicPlayer", "Invalid track index for album art: " + std::to_string(trackIndex));
			return false;
		}

		// Get the file path of the requested track
		std::string filePath = musicFiles_[trackIndex];

		// Check if file exists
		if (!std::filesystem::exists(filePath)) {
			LOG_ERROR("MusicPlayer", "Track file does not exist: " + filePath);
			return false;
		}

		// Extract album art data from the file
		bool result = extractAlbumArtFromFile(filePath, albumArtData);

		if (!result || albumArtData.empty()) {
			LOG_INFO("MusicPlayer", "No album art found in track: " + musicNames_[trackIndex]);
			return false;
		}

		LOG_INFO("MusicPlayer", "Extracted album art from track: " + musicNames_[trackIndex]);
		return true;
	}
	catch (const std::exception& e) {
		LOG_ERROR("MusicPlayer", "Exception getting album art: " + std::string(e.what()));
		return false;
	}
	catch (...) {
		LOG_ERROR("MusicPlayer", "Unknown exception getting album art");
		return false;
	}
}

double MusicPlayer::getCurrent() {
	if (!currentMusic_) {
		return -1.0;
	}

	return Mix_GetMusicPosition(currentMusic_);
}

double MusicPlayer::getDuration() {
	if (!currentMusic_) {
		return -1.0;
	}

	return Mix_MusicDuration(currentMusic_);
}

std::pair<int, int> MusicPlayer::getCurrentAndDurationSec() {
	if (!currentMusic_) return { -1, -1 };
	return {
		static_cast<int>(Mix_GetMusicPosition(currentMusic_)),
		static_cast<int>(Mix_MusicDuration(currentMusic_))
	};
}

bool MusicPlayer::isFading() const {
	return Mix_FadingMusic() != MIX_NO_FADING;
}

bool MusicPlayer::hasStartedPlaying() const {
	return hasStartedPlaying_;
}

void MusicPlayer::setButtonPressed(bool buttonPressed) {
	buttonPressed_ = buttonPressed;
}

bool MusicPlayer::getButtonPressed() {
	return buttonPressed_;
}

int MusicPlayer::getSampleSize() const {
	return sampleSize_;
}