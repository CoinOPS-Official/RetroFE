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

    // Load music folder path from config
    std::string musicFolder;
    if (config.getProperty("musicPlayer.folder", musicFolder))
    {
        loadMusicFolder(musicFolder);
    }
    else
    {
        // Default to a music directory in RetroFE's path
        loadMusicFolder(Utils::combinePath(Configuration::absolutePath, "music"));
    }

    LOG_INFO("MusicPlayer", "Initialized with volume: " + std::to_string(volume) +
        ", loop: " + std::to_string(loopMode) +
        ", shuffle: " + std::to_string(shuffleMode) +
        ", tracks found: " + std::to_string(musicFiles.size()));

    return true;
}

bool MusicPlayer::loadMusicFolder(const std::string& folderPath)
{
    // Clear existing music files
    musicFiles.clear();
    musicNames.clear();

    LOG_INFO("MusicPlayer", "Loading music from folder: " + folderPath);

    try
    {
        if (!fs::exists(folderPath))
        {
            LOG_WARNING("MusicPlayer", "Music folder doesn't exist: " + folderPath);
            return false;
        }

        for (const auto& entry : fs::directory_iterator(folderPath))
        {
            if (entry.is_regular_file())
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".mod")
                {
                    musicFiles.push_back(entry.path().string());
                    musicNames.push_back(entry.path().filename().string());
                }
            }
        }

        // Sort alphabetically
        std::vector<std::pair<std::string, std::string>> combined;
        for (size_t i = 0; i < musicFiles.size(); ++i)
        {
            combined.push_back({ musicFiles[i], musicNames[i] });
        }

        std::sort(combined.begin(), combined.end(),
            [](const auto& a, const auto& b) {
                return a.second < b.second;
            });

        musicFiles.clear();
        musicNames.clear();
        for (const auto& pair : combined)
        {
            musicFiles.push_back(pair.first);
            musicNames.push_back(pair.second);
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

    LOG_INFO("MusicPlayer", "Playing track: " + musicNames[index]);
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
    LOG_INFO("MusicPlayer", "Track finished: " + getCurrentTrackName());

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