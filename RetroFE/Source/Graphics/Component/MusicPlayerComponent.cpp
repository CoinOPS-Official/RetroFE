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
	, currentPage_(&p)
	, config_(config)
	, commonMode_(commonMode)
	, loadedComponent_(nullptr)
	, type_(type)
	, musicPlayer_(MusicPlayer::getInstance())
	, font_(font)
	, lastState_("")
	, refreshInterval_(0.5f)
	, refreshTimer_(0.0f)
	, albumArtTexture_(nullptr)
	, albumArtTrackIndex_(-1)
	, renderer_(nullptr)
	, albumArtTextureWidth_(0)
	, albumArtTextureHeight_(0)
	, isAlbumArt_(Utils::toLower(type) == "albumart")
	, volumeEmptyTexture_(nullptr)
	, volumeFullTexture_(nullptr)
	, volumeBarTexture_(nullptr)
	, volumeBarWidth_(0)
	, volumeBarHeight_(0)
	, lastVolumeValue_(-1)
	, isVolumeBar_(Utils::toLower(type) == "volbar")
	, currentDisplayAlpha_(0.0f)  // Start invisible
	, targetAlpha_(0.0f)          // Start with target of invisible
	, fadeSpeed_(3.0f)            // Fade in/out in about 1/3 second
	, volumeStableTimer_(0.0f)    // Reset timer
	, volumeFadeDelay_(1.5f)      // Wait 1.5 seconds before fading out
	, volumeChanging_(false)      // Not changing initially
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

	// Clean up volume bar textures
	if (volumeEmptyTexture_ != nullptr) {
		SDL_DestroyTexture(volumeEmptyTexture_);
		volumeEmptyTexture_ = nullptr;
	}

	if (volumeFullTexture_ != nullptr) {
		SDL_DestroyTexture(volumeFullTexture_);
		volumeFullTexture_ = nullptr;
	}

	if (volumeBarTexture_ != nullptr) {
		SDL_DestroyTexture(volumeBarTexture_);
		volumeBarTexture_ = nullptr;
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

	// Get the renderer if we're going to handle album art or volume bar
	if (isAlbumArt_ || isVolumeBar_) {
		renderer_ = SDL::getRenderer(baseViewInfo.Monitor);
	}

	// If this is a volume bar, load the necessary textures
	if (isVolumeBar_) {
		// Load volume bar textures
		loadVolumeBarTextures();

		// Load user configuration for fade duration (in milliseconds)
		int fadeDurationMs = 333; // Default: 333ms (1/3 second)

		// Try to get user-defined fade duration from config
		if (config_.getProperty("volumeBar.fadeDuration", fadeDurationMs) ||
			config_.getProperty("musicPlayer.volumeBar.fadeDuration", fadeDurationMs)) {
			// Ensure value is at least 1ms to avoid division by zero
			fadeDurationMs = std::max(1, fadeDurationMs);

			// Convert from milliseconds to seconds, then to fadeSpeed (which is 1/duration)
			float fadeDurationSeconds = static_cast<float>(fadeDurationMs) / 1000.0f;
			fadeSpeed_ = 1.0f / fadeDurationSeconds;

			// Log the setting
			LOG_INFO("MusicPlayerComponent",
				"Volume bar fade duration set to " + std::to_string(fadeDurationMs) + "ms");
		}

		int fadeDelayMs = 1500; // Default: 1500ms (1.5 seconds)
		if (config_.getProperty("volumeBar.fadeDelay", fadeDelayMs) ||
			config_.getProperty("musicPlayer.volumeBar.fadeDelay", fadeDelayMs)) {
			// Convert from milliseconds to seconds
			volumeFadeDelay_ = static_cast<float>(fadeDelayMs) / 1000.0f;
		}
		// Don't create a loadedComponent for volbar type since we handle it directly
	}
	// Only create loadedComponent if this isn't a special type we handle directly
	else {
		// Create the component based on the specified type
		loadedComponent_ = reloadComponent();

		if (loadedComponent_ != nullptr) {
			loadedComponent_->allocateGraphicsMemory();
		}
	}
}

void MusicPlayerComponent::loadVolumeBarTextures()
{
	// Get layout name from config
	std::string layoutName;
	config_.getProperty(OPTION_LAYOUT, layoutName);

	// Base paths for volume bar images
	std::vector<std::string> searchPaths;

	// If we have a collection name, look there first
	std::string collectionName;
	if (config_.getProperty("collection", collectionName) && !collectionName.empty()) {
		searchPaths.push_back(Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
			"collections", collectionName, "volbar"));
	}

	// Then check common locations
	searchPaths.push_back(Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
		"collections", "_common", "medium_artwork", "volbar"));
	searchPaths.push_back(Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "volbar"));

	// Find empty and full images
	std::string emptyPath;
	std::string fullPath;

	for (const auto& basePath : searchPaths) {
		// Look for empty.png/jpg
		std::vector<std::string> extensions = { ".png", ".jpg", ".jpeg" };
		for (const auto& ext : extensions) {
			std::string path = Utils::combinePath(basePath, "empty" + ext);
			if (std::filesystem::exists(path)) {
				emptyPath = path;
				break;
			}
		}

		// Look for full.png/jpg
		for (const auto& ext : extensions) {
			std::string path = Utils::combinePath(basePath, "full" + ext);
			if (std::filesystem::exists(path)) {
				fullPath = path;
				break;
			}
		}

		if (!emptyPath.empty() && !fullPath.empty()) {
			break; // Found both, no need to check further paths
		}
	}

	// If we couldn't find the images, log an error
	if (emptyPath.empty() || fullPath.empty()) {
		LOG_ERROR("MusicPlayerComponent", "Could not find empty.png and full.png for volume bar");
		return;
	}

	// Load textures
	volumeEmptyTexture_ = IMG_LoadTexture(renderer_, emptyPath.c_str());
	volumeFullTexture_ = IMG_LoadTexture(renderer_, fullPath.c_str());

	if (!volumeEmptyTexture_ || !volumeFullTexture_) {
		LOG_ERROR("MusicPlayerComponent", "Failed to load volume bar textures");
		if (volumeEmptyTexture_) {
			SDL_DestroyTexture(volumeEmptyTexture_);
			volumeEmptyTexture_ = nullptr;
		}
		if (volumeFullTexture_) {
			SDL_DestroyTexture(volumeFullTexture_);
			volumeFullTexture_ = nullptr;
		}
		return;
	}

	// Get texture dimensions - both should have the same size
	SDL_QueryTexture(volumeFullTexture_, nullptr, nullptr, &volumeBarWidth_, &volumeBarHeight_);

	// Create the render target texture
	volumeBarTexture_ = SDL_CreateTexture(
		renderer_,
		SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_TARGET,
		volumeBarWidth_,
		volumeBarHeight_
	);

	if (volumeBarTexture_) {
		SDL_SetTextureBlendMode(volumeBarTexture_, SDL_BLENDMODE_BLEND);
	}
	else {
		LOG_ERROR("MusicPlayerComponent", "Failed to create volume bar render target texture");
	}

	// Initial update of the texture
	updateVolumeBarTexture();
}

void MusicPlayerComponent::updateVolumeBarTexture()
{
	if (!renderer_ || !volumeEmptyTexture_ || !volumeFullTexture_ || !volumeBarTexture_) {
		return;
	}

	// Get current volume (0-128) and convert to percentage
	int volumeRaw = musicPlayer_->getVolume();
	float volumePercent = static_cast<float>(volumeRaw) / MIX_MAX_VOLUME;

	// Calculate the width of the visible portion of the full texture
	int visibleWidth = static_cast<int>(volumeBarWidth_ * volumePercent);

	// Set render target to our texture
	SDL_SetRenderTarget(renderer_, volumeBarTexture_);

	// Clear the texture
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
	SDL_RenderClear(renderer_);

	// Draw the full texture first (visible part based on volume)
	if (visibleWidth > 0) {
		SDL_Rect srcRect = { 0, 0, visibleWidth, volumeBarHeight_ };
		SDL_Rect destRect = { 0, 0, visibleWidth, volumeBarHeight_ };
		SDL_RenderCopy(renderer_, volumeFullTexture_, &srcRect, &destRect);
	}

	// Draw the empty texture for the remaining portion
	if (visibleWidth < volumeBarWidth_) {
		SDL_Rect srcRect = { visibleWidth, 0, volumeBarWidth_ - visibleWidth, volumeBarHeight_ };
		SDL_Rect destRect = { visibleWidth, 0, volumeBarWidth_ - visibleWidth, volumeBarHeight_ };
		SDL_RenderCopy(renderer_, volumeEmptyTexture_, &srcRect, &destRect);
	}

	// Reset render target
	SDL_SetRenderTarget(renderer_, nullptr);

	// Update last volume value
	lastVolumeValue_ = volumeRaw;
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
	if (isAlbumArt_) {
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

	// Special handling for volume bar
	if (isVolumeBar_) {
		// Check if volume has changed
		int currentVolume = musicPlayer_->getVolume();
		if (currentVolume != lastVolumeValue_) {
			// Volume changed - update the texture and animation state
			updateVolumeBarTexture();
			volumeChanging_ = true;
			volumeStableTimer_ = 0.0f;
		}
		else {
			// Volume is stable
			volumeStableTimer_ += dt;

			// If volume has been stable for the fade delay, start fading out
			if (volumeChanging_ && volumeStableTimer_ >= volumeFadeDelay_) {
				volumeChanging_ = false;
			}
		}

		// Set target alpha based on current state and baseViewInfo.Alpha
		if (volumeChanging_) {
			// When volume is changing, target is the current baseViewInfo.Alpha
			// This ensures we always track changes to baseViewInfo.Alpha
			targetAlpha_ = baseViewInfo.Alpha;
		}
		else {
			// When volume is stable and we've passed the delay, target is 0
			targetAlpha_ = 0.0f;
		}

		// Animate the alpha with consistent timing
		if (currentDisplayAlpha_ != targetAlpha_) {
			// Calculate the maximum change amount for this frame to maintain consistent speed
			float maxAlphaChange = dt * fadeSpeed_;

			if (currentDisplayAlpha_ < targetAlpha_) {
				// Fade in
				float alphaChange = std::min(targetAlpha_ - currentDisplayAlpha_, maxAlphaChange);
				currentDisplayAlpha_ += alphaChange;
			}
			else {
				// Fade out
				float alphaChange = std::min(currentDisplayAlpha_ - targetAlpha_, maxAlphaChange);
				currentDisplayAlpha_ -= alphaChange;
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
	else if (type_ == "volume") {

	}
	else {
		// For track/artist/album types, use the currently playing track
		currentState = musicPlayer_->getFormattedTrackInfo();
	}

	// Check if update is needed (state changed or refresh interval elapsed)
	bool needsUpdate = (currentState != lastState_) 
		|| (refreshTimer_ >= refreshInterval_) 
		|| type_ == "volume";

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
	if (isAlbumArt_) {
		if (baseViewInfo.Alpha > 0.0f) {
			drawAlbumArt();
		}
		return;
	}

	// For volume bar, handle drawing directly
	if (isVolumeBar_) {
		if (baseViewInfo.Alpha > 0.0f) {
			drawVolumeBar();
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
					baseViewInfo.ImageWidth = static_cast<float>(albumArtTextureWidth_);
					baseViewInfo.ImageHeight = static_cast<float>(albumArtTextureHeight_);
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
		SDL_FRect rect;

		// Use the baseViewInfo for position and size calculations
		rect.x = baseViewInfo.XRelativeToOrigin();
		rect.y = baseViewInfo.YRelativeToOrigin();
		rect.h = baseViewInfo.ScaledHeight();
		rect.w = baseViewInfo.ScaledWidth();

		// Use the existing SDL render method
		SDL::renderCopyF(
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

void MusicPlayerComponent::drawVolumeBar()
{
	if (!renderer_ || !volumeBarTexture_ || baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	// Draw the volume bar texture
	SDL_FRect rect;

	// Use the baseViewInfo for position and size calculations
	rect.x = baseViewInfo.XRelativeToOrigin();
	rect.y = baseViewInfo.YRelativeToOrigin();

	// Use the specified dimensions in the layout, or the texture dimensions if not specified
	if (baseViewInfo.Width > 0) {
		rect.w = baseViewInfo.ScaledWidth();
	}
	else {
		rect.w = static_cast<float>(volumeBarWidth_);
	}

	if (baseViewInfo.Height > 0) {
		rect.h = baseViewInfo.ScaledHeight();
	}
	else {
		rect.h = static_cast<float>(volumeBarHeight_);
	}

	SDL::renderCopyF(
		volumeBarTexture_,
		currentDisplayAlpha_,
		nullptr,
		&rect,
		baseViewInfo,
		page.getLayoutWidth(baseViewInfo.Monitor),
		page.getLayoutHeight(baseViewInfo.Monitor)
	);
}

Component* MusicPlayerComponent::reloadComponent()
{
	// Album art is handled directly, don't create a component for it
	if (isAlbumArt_ || isVolumeBar_) {
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
	else if (typeLC == "volume") {
		int volumeRaw = musicPlayer_->getVolume();
		int volumePercentage = static_cast<int>((volumeRaw / 128.0f) * 100.0f + 0.5f);
		std::string volumeStr = std::to_string(volumePercentage);

		return new Text(volumeStr, page, font_, baseViewInfo.Monitor);
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