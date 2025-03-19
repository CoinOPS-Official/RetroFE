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

#include "MusicPlayerComponent.h"
#include "ImageBuilder.h"
#include "Text.h"
#include "Image.h"
#include "../Page.h"
#include "../ViewInfo.h"
#include "../../Sound/MusicPlayer.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include <sstream>
#include <iomanip>

MusicPlayerComponent::MusicPlayerComponent(Configuration& config, bool commonMode, const std::string& type, Page& p, int monitor, FontManager* font)
    : Component(p)
    , config_(config)
    , commonMode_(commonMode)
    , loadedComponent_(nullptr)
    , type_(type)
    , musicPlayer_(MusicPlayer::getInstance())
    , font_(font)
    , lastState_("")
    , refreshInterval_(1.0f)
    , refreshTimer_(0.0f)
    , albumArtTexture_(nullptr)
    , albumArtTrackIndex_(-1)
    , renderer_(nullptr)
    , albumArtTextureWidth_(0)
    , albumArtTextureHeight_(0)
{
    // Set the monitor for this component
    baseViewInfo.Monitor = monitor;

    // Get refresh interval from config if available
    int configRefreshInterval;
    if (config.getProperty("musicPlayer.refreshRate", configRefreshInterval)) {
        refreshInterval_ = static_cast<float>(configRefreshInterval) / 1000.0f; // Convert from ms to seconds
    }

    allocateGraphicsMemory();
}

MusicPlayerComponent::~MusicPlayerComponent()
{
    freeGraphicsMemory();
}

void MusicPlayerComponent::freeGraphicsMemory()
{
    Component::freeGraphicsMemory();

    if (albumArtTexture_ != nullptr) {
        SDL_DestroyTexture(albumArtTexture_);
        albumArtTexture_ = nullptr;
    }

    if (loadedComponent_ != nullptr) {
        loadedComponent_->freeGraphicsMemory();
        delete loadedComponent_;
        loadedComponent_ = nullptr;
    }
}

void MusicPlayerComponent::allocateGraphicsMemory()
{
    Component::allocateGraphicsMemory();

    // Get the renderer if we're going to handle album art
    if (Utils::toLower(type_) == "albumart") {
        renderer_ = SDL::getRenderer(baseViewInfo.Monitor);
    }

    // Create the component based on the specified type
    loadedComponent_ = reloadComponent();

    if (loadedComponent_ != nullptr) {
        loadedComponent_->allocateGraphicsMemory();
    }
}

std::string_view MusicPlayerComponent::filePath()
{
    if (loadedComponent_ != nullptr) {
        return loadedComponent_->filePath();
    }
    return "";
}

bool MusicPlayerComponent::update(float dt)
{
    // Update refresh timer
    refreshTimer_ += dt;

    // Special handling for album art
    if (Utils::toLower(type_) == "albumart") {
        int currentTrackIndex = musicPlayer_->getCurrentTrackIndex();

        // Check if track has changed or refresh timeout
        bool needsUpdate = (currentTrackIndex != albumArtTrackIndex_) || (refreshTimer_ >= refreshInterval_);

        if (needsUpdate) {
            refreshTimer_ = 0.0f;

            // If track changed, reset texture reference to trigger reload
            if (currentTrackIndex != albumArtTrackIndex_) {
                // Free old texture if needed
                if (albumArtTexture_ != nullptr) {
                    SDL_DestroyTexture(albumArtTexture_);
                    albumArtTexture_ = nullptr;
                }

                albumArtTrackIndex_ = currentTrackIndex;
                lastState_ = std::to_string(currentTrackIndex); // Update state to track index
            }
        }

        return Component::update(dt);
    }

    // Determine current state
    std::string currentState;

    if (type_ == "state") {
        currentState = musicPlayer_->isPlaying() ? "playing" : "paused";
    }
    else if (type_ == "shuffle") {
        currentState = musicPlayer_->getShuffle() ? "on" : "off";
    }
    else if (type_ == "loop") {
        currentState = musicPlayer_->getLoop() ? "on" : "off";
    }
    else if (type_ == "time") {
        // For time, update on every refresh interval
        //currentState = std::to_string(musicPlayer_->getCurrent());
    }
    else {
        // For track/artist/album types, use the currently playing track
        currentState = musicPlayer_->getFormattedTrackInfo();
    }

    // Check if update is needed (state changed or refresh interval elapsed)
    bool needsUpdate = (currentState != lastState_) || (refreshTimer_ >= refreshInterval_);

    if (needsUpdate) {
        // Reset timer
        refreshTimer_ = 0.0f;

        // Update state tracking
        lastState_ = currentState;

        // Recreate the component based on current state
        Component* newComponent = reloadComponent();

        if (newComponent != nullptr) {
            // Replace existing component if needed
            if (newComponent != loadedComponent_) {
                if (loadedComponent_ != nullptr) {
                    loadedComponent_->freeGraphicsMemory();
                    delete loadedComponent_;
                }
                loadedComponent_ = newComponent;
                loadedComponent_->allocateGraphicsMemory();
            }
        }
    }

    // Update the loaded component
    if (loadedComponent_ != nullptr) {
        loadedComponent_->update(dt);
    }

    return Component::update(dt);
}

void MusicPlayerComponent::draw()
{
    Component::draw();

    // For album art, handle drawing directly
    if (Utils::toLower(type_) == "albumart") {
        if (baseViewInfo.Alpha > 0.0f) {
            drawAlbumArt();
        }
        return;
    }

    if (loadedComponent_ != nullptr) {
        loadedComponent_->baseViewInfo = baseViewInfo;
        if (baseViewInfo.Alpha > 0.0f) {
            loadedComponent_->draw();
        }
    }
}

void MusicPlayerComponent::drawAlbumArt()
{
    if (!renderer_ || baseViewInfo.Alpha <= 0.0f) {
        return;
    }

    // Try to get album art texture if we don't have one
    if (albumArtTexture_ == nullptr && albumArtTrackIndex_ >= 0) {
        // Get album art from the music player
        std::vector<unsigned char> albumArtData;
        if (musicPlayer_->getAlbumArt(albumArtTrackIndex_, albumArtData) && !albumArtData.empty()) {
            // Convert album art data to texture using SDL_image
            SDL_RWops* rw = SDL_RWFromConstMem(albumArtData.data(), static_cast<int>(albumArtData.size()));
            if (rw) {
                // Use IMG_LoadTexture_RW which simplifies the process
                albumArtTexture_ = IMG_LoadTexture_RW(renderer_, rw, 1); // 1 means auto-close

                if (albumArtTexture_) {
                    // Get texture dimensions
                    SDL_QueryTexture(albumArtTexture_, nullptr, nullptr,
                        &albumArtTextureWidth_, &albumArtTextureHeight_);
                    baseViewInfo.ImageWidth = (float)albumArtTextureWidth_;
                    baseViewInfo.ImageHeight = (float)albumArtTextureHeight_;
                    LOG_INFO("MusicPlayerComponent", "Created album art texture");
                }
            }
        }

        // If no album art found or texture creation failed, try to load default
        if (albumArtTexture_ == nullptr) {
            albumArtTexture_ = loadDefaultAlbumArt();
        }
    }

    // Draw the album art if we have a texture
    if (albumArtTexture_ != nullptr) {
        SDL_Rect rect;

        // Use the baseViewInfo for position and size calculations
        rect.x = static_cast<int>(baseViewInfo.XRelativeToOrigin());
        rect.y = static_cast<int>(baseViewInfo.YRelativeToOrigin());
        rect.h = static_cast<int>(baseViewInfo.ScaledHeight());
        rect.w = static_cast<int>(baseViewInfo.ScaledWidth());

        // Use the existing SDL render method
        SDL::renderCopy(
            albumArtTexture_,
            baseViewInfo.Alpha,
            nullptr,
            &rect,
            baseViewInfo,
            page.getLayoutWidth(baseViewInfo.Monitor),
            page.getLayoutHeight(baseViewInfo.Monitor)
        );
    }
}

SDL_Texture* MusicPlayerComponent::loadDefaultAlbumArt()
{
    // Get layout name from config
    std::string layoutName;
    config_.getProperty(OPTION_LAYOUT, layoutName);

    // Try different paths for default album art
    std::vector<std::string> searchPaths = {
        Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
                          "collections", "_common", "medium_artwork", "albumart", "default.png"),
        Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
                          "collections", "_common", "medium_artwork", "albumart", "default.jpg"),
        Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
                          "collections", "_common", "medium_artwork", "music", "default.png"),
        Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
                          "collections", "_common", "medium_artwork", "music", "default.jpg")
    };

    // Try to load each path using IMG_LoadTexture which simplifies the process
    for (const auto& path : searchPaths) {
        if (std::filesystem::exists(path)) {
            SDL_Texture* texture = IMG_LoadTexture(renderer_, path.c_str());
            if (texture) {
                // Get dimensions for the default texture
                SDL_QueryTexture(texture, nullptr, nullptr,
                    &albumArtTextureWidth_, &albumArtTextureHeight_);
                LOG_INFO("MusicPlayerComponent", "Loaded default album art from: " + path);
                return texture;
            }
        }
    }

    LOG_WARNING("MusicPlayerComponent", "Failed to load default album art");
    return nullptr;
}

Component* MusicPlayerComponent::reloadComponent()
{
    // Album art is handled directly, don't create a component for it
    if (Utils::toLower(type_) == "albumart") {
        return nullptr;
    }
    
    Component* component = nullptr;
    std::string typeLC = Utils::toLower(type_);
    std::string basename;

    // Determine the basename based on component type
    if (typeLC == "state") {
        // Check if we need to reset the direction - do this when fading has completed
        MusicPlayer::TrackChangeDirection direction = musicPlayer_->getTrackChangeDirection();

        // If we have a direction set and fading has completed, reset the direction
        if (direction != MusicPlayer::TrackChangeDirection::NONE && !musicPlayer_->isFading()) {
            // Only reset if we're actually playing music (not in a paused state)
            if (musicPlayer_->isPlaying()) {
                musicPlayer_->setTrackChangeDirection(MusicPlayer::TrackChangeDirection::NONE);
            }
        }

        // Get the potentially updated direction after reset check
        direction = musicPlayer_->getTrackChangeDirection();

        // Set basename based on priority: direction indicators first, then play state
        if (direction == MusicPlayer::TrackChangeDirection::NEXT) {
            basename = "next";
        }
        else if (direction == MusicPlayer::TrackChangeDirection::PREVIOUS) {
            basename = "previous";
        }
        else if (musicPlayer_->isPlaying()) {
            basename = "playing";
        }
        else if (musicPlayer_->isPaused()) {
            basename = "paused";
        }
    }
    else if (typeLC == "shuffle") {
        basename = musicPlayer_->getShuffle() ? "on" : "off";
    }
    else if (typeLC == "loop" || typeLC == "repeat") {
        basename = musicPlayer_->getLoop() ? "on" : "off";
    }
    else if (typeLC == "filename") {
        std::string fileName = musicPlayer_->getCurrentTrackNameWithoutExtension();
        if (fileName.empty()) {
            fileName = "";
        }
        return new Text(fileName, page, font_, baseViewInfo.Monitor);
    }
    else if (typeLC == "trackinfo") {
        // For track text, create a Text component directly
        std::string trackName = musicPlayer_->getFormattedTrackInfo();
        if (trackName.empty()) {
            trackName = "No track playing";
        }
        return new Text(trackName, page, font_, baseViewInfo.Monitor);
    }
    else if (typeLC == "title") {
        std::string titleName = musicPlayer_->getCurrentTitle();
        if (titleName.empty()) {
            titleName = "Unknown";
        }
        return new Text(titleName, page, font_, baseViewInfo.Monitor);
    }
    else if (typeLC == "artist") {
        std::string artistName = musicPlayer_->getCurrentArtist();
        if (artistName.empty()) {
            artistName = "Unknown Artist";
        }
        return new Text(artistName, page, font_, baseViewInfo.Monitor);
    }
    else if (typeLC == "album") {
        std::string albumName = musicPlayer_->getCurrentAlbum();
        if (albumName.empty()) {
            albumName = "Unknown Album";
        }
        return new Text(albumName, page, font_, baseViewInfo.Monitor);
    }
    else if (typeLC == "time") {
        // Format time based on duration length
        int currentSec = static_cast<int>(musicPlayer_->getCurrent());
        int durationSec = static_cast<int>(musicPlayer_->getDuration());

        if (currentSec < 0)
            return nullptr;

        // Calculate minutes and remaining seconds
        int currentMin = currentSec / 60;
        int currentRemSec = currentSec % 60;
        int durationMin = durationSec / 60;
        int durationRemSec = durationSec % 60;

        std::stringstream ss;

        // Determine if we need to pad minutes with zeros based on duration minutes
        int minWidth = 1; // Default no padding

        // If duration minutes is 10 or more, use padding
        if (durationMin >= 10) {
            minWidth = 2; // Use 2 digits for minutes
        }

        // Format minutes with conditional padding
        ss << std::setfill('0') << std::setw(minWidth) << currentMin << ":"
            << std::setfill('0') << std::setw(2) << currentRemSec // Seconds always use 2 digits
            << "/"
            << std::setfill('0') << std::setw(minWidth) << durationMin << ":"
            << std::setfill('0') << std::setw(2) << durationRemSec;

        return new Text(ss.str(), page, font_, baseViewInfo.Monitor);
    }
    else if (typeLC == "progress") {
 
    }
    else {
        // Default basename for other types
        basename = typeLC;
    }

    // Get the layout name from configuration
    std::string layoutName;
    config_.getProperty(OPTION_LAYOUT, layoutName);

    // Construct path to the image
    std::string imagePath;
    if (commonMode_) {
        // Use common path for music player components
        imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", "_common", "medium_artwork", typeLC);
    }
    else {
        // Use a specific path if not in common mode
        imagePath = Utils::combinePath(Configuration::absolutePath, "music", typeLC);
    }

    // Use ImageBuilder to create the image component
    ImageBuilder imageBuild{};
    component = imageBuild.CreateImage(imagePath, page, basename, baseViewInfo.Monitor, baseViewInfo.Additive, true);

    return component;
}

// Forward control functions to the music player
void MusicPlayerComponent::skipForward()
{
    //musicPlayer_->next();
}

void MusicPlayerComponent::skipBackward()
{
    //musicPlayer_->previous();
}

void MusicPlayerComponent::skipForwardp()
{
    // Fast forward - seek 10 seconds forward if supported
    //unsigned long long current = musicPlayer_->getCurrent();
    //musicPlayer_->seekTo(current + 10000); // 10 seconds
}

void MusicPlayerComponent::skipBackwardp()
{
    // Rewind - seek 10 seconds backward if supported
    //unsigned long long current = musicPlayer_->getCurrent();
    //if (current > 10000) {
        //musicPlayer_->seekTo(current - 10000); // 10 seconds
    //}
    //else {
        //musicPlayer_->seekTo(0); // Beginning of track
    //}
}

void MusicPlayerComponent::pause()
{
    if (musicPlayer_->isPlaying()) {
        musicPlayer_->pauseMusic();
    }
    else {
        musicPlayer_->playMusic();
    }
}

void MusicPlayerComponent::restart()
{
    //musicPlayer_->seekTo(0); // Go to beginning of track
    //if (!musicPlayer_->isPlaying()) {
    //    musicPlayer_->play();
    //}
}

unsigned long long MusicPlayerComponent::getCurrent()
{
    return 1;
    //return musicPlayer_->getCurrent();
}

unsigned long long MusicPlayerComponent::getDuration()
{
    return 1;
    //return musicPlayer_->getDuration();
}

bool MusicPlayerComponent::isPaused()
{
    return musicPlayer_->isPaused();
}

bool MusicPlayerComponent::isPlaying()
{
    return musicPlayer_->isPlaying();
}