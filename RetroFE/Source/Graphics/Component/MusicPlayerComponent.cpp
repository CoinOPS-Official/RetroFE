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
#include <algorithm>
#include <charconv>

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
	, refreshInterval_(0.25f)
	, refreshTimer_(0.0f)
	, directionDisplayTimer_(0.0f)
	, directionDisplayDuration_(0.5f)
	, albumArtTexture_(nullptr)
	, albumArtTrackIndex_(-1)
	, renderer_(nullptr)
	, albumArtTextureWidth_(0)
	, albumArtTextureHeight_(0)
	, albumArtNeedsUpdate_{ false }
	, isAlbumArt_(Utils::toLower(type) == "albumart")
	, volumeEmptyTexture_(nullptr)
	, volumeFullTexture_(nullptr)
	, volumeBarTexture_(nullptr)
	, volumeBarWidth_(0)
	, volumeBarHeight_(0)
	, lastVolumeValue_(-1)
	, volumeBarNeedsUpdate_{ false }
	, isVolumeBar_(Utils::toLower(type) == "volbar")
	, isProgressBar_(Utils::toLower(type) == "progress")
	, currentDisplayAlpha_(0.0f)  // Start invisible
	, targetAlpha_(0.0f)          // Start with target of invisible
	, fadeSpeed_(3.0f)            // Fade in/out in about 1/3 second
	, volumeStableTimer_(0.0f)    // Reset timer
	, volumeFadeDelay_(1.5f)      // Wait 1.5 seconds before fading out
	, volumeChanging_(false)      // Not changing initially
	, isVuMeter_(Utils::toLower(type) == "vumeter")
	, vuBarCount_(40)  // Default number of bars
	, vuDecayRate_(2.0f)  // How quickly the bars fall
	, vuPeakDecayRate_(0.4f)  // How quickly the peak markers fall
	, vuMeterTexture_(nullptr)
	, vuMeterTextureWidth_(0)
	, vuMeterTextureHeight_(0)
	, vuMeterNeedsUpdate_(true)
	, vuMeterIsMono_(false) // Initialize to default (stereo)
	, vuBottomColor_({ 0, 220, 0, 255 })
	, vuMiddleColor_({ 220, 220, 0, 255 })
	, vuTopColor_({ 220, 0, 0, 255 })
	, vuBackgroundColor_({ 40, 40, 40, 255 })
	, vuPeakColor_({ 255, 255, 255, 255 })
	, vuGreenThreshold_(0.4f)
	, vuYellowThreshold_(0.6f)
	, totalSegments_{ 0 }
	, useSegmentedVolume_{ false } {
	// Set the monitor for this component
	baseViewInfo.Monitor = monitor;

	// Get refresh interval from config if available
	int configRefreshInterval;
	if (config.getProperty("musicPlayer.refreshRate", configRefreshInterval)) {
		refreshInterval_ = static_cast<float>(configRefreshInterval) / 1000.0f; // Convert from ms to seconds
	}

	allocateGraphicsMemory();
}

MusicPlayerComponent::~MusicPlayerComponent() {
	freeGraphicsMemory();
}

void MusicPlayerComponent::freeGraphicsMemory() {
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

	if (vuMeterTexture_ != nullptr) {
		SDL_DestroyTexture(vuMeterTexture_);
		vuMeterTexture_ = nullptr;
	}

	if (loadedComponent_ != nullptr) {
		loadedComponent_->freeGraphicsMemory();
		delete loadedComponent_;
		loadedComponent_ = nullptr;
	}
}

void MusicPlayerComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();

	// Get the renderer if we're going to handle album art or volume bar
	if (isAlbumArt_ || isVolumeBar_ || isProgressBar_ || isVuMeter_) {
		renderer_ = SDL::getRenderer(baseViewInfo.Monitor);
	}

	if (isVuMeter_) {
		musicPlayer_->registerVisualizerCallback();
		
		config_.getProperty("musicPlayer.vuMeter.mono", vuMeterIsMono_); // Reads boolean, defaults to false if not found
		if (vuMeterIsMono_) {
			LOG_INFO("MusicPlayerComponent", "VU Meter configured for mono display.");
		}
		else {
			LOG_INFO("MusicPlayerComponent", "VU Meter configured for stereo display (default).");
		}
		
		int configBarCount;
		if (config_.getProperty("musicPlayer.vuMeter.barCount", configBarCount)) {
			vuBarCount_ = std::max(1, std::min(32, configBarCount)); // Limit to reasonable range
		}

		float configDecayRate;
		if (config_.getProperty("musicPlayer.vuMeter.decayRate", configDecayRate)) {
			vuDecayRate_ = std::max(0.1f, configDecayRate);
		}

		float configPeakDecayRate;
		if (config_.getProperty("musicPlayer.vuMeter.peakDecayRate", configPeakDecayRate)) {
			vuPeakDecayRate_ = std::max(0.1f, configPeakDecayRate);
		}

		// Load color configurations
		std::string colorStr;
		SDL_Color parsedColor; // Temporary variable for parsing result

		// Load Bottom Color
		if (config_.getProperty("musicPlayer.vuMeter.bottomColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuBottomColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.bottomColor: '" + colorStr + "'. Using default.");
			}
		} // else: default vuBottomColor_ is used

		// Load Middle Color
		if (config_.getProperty("musicPlayer.vuMeter.middleColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuMiddleColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.middleColor: '" + colorStr + "'. Using default.");
			}
		} // else: default vuMiddleColor_ is used

		// Load Top Color
		if (config_.getProperty("musicPlayer.vuMeter.topColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuTopColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.topColor: '" + colorStr + "'. Using default.");
			}
		} // else: default vuTopColor_ is used

		// Load Background Color
		if (config_.getProperty("musicPlayer.vuMeter.backgroundColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuBackgroundColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.backgroundColor: '" + colorStr + "'. Using default.");
			}
		} // else: default vuBackgroundColor_ is used

		// Load Peak Color
		if (config_.getProperty("musicPlayer.vuMeter.peakColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuPeakColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.peakColor: '" + colorStr + "'. Using default.");
			}
		} // else: default vuPeakColor_ is used

		// Load thresholds
		float threshold;
		if (config_.getProperty("musicPlayer.vuMeter.greenThreshold", threshold)) {
			vuGreenThreshold_ = std::max(0.0f, std::min(1.0f, threshold));
		}

		if (config_.getProperty("musicPlayer.vuMeter.yellowThreshold", threshold)) {
			vuYellowThreshold_ = std::max(0.0f, std::min(1.0f, threshold));
			// Ensure yellow threshold is greater than green
			vuYellowThreshold_ = std::max(vuYellowThreshold_, vuGreenThreshold_);
		}

		// Initialize the VU level arrays
		vuLevels_.resize(vuBarCount_, 0.0f);
		vuPeaks_.resize(vuBarCount_, 0.0f);
	}


	// If this is a volume bar, load the necessary textures
	if (isVolumeBar_) {
		// Load volume bar textures
		loadVolumeBarTextures();

		// Load user configuration for fade duration (in milliseconds)
		int fadeDurationMs = 333; // Default: 333ms (1/3 second)

		// Try to get user-defined fade duration from config
		if (config_.getProperty("musicPlayer.volumeBar.fadeDuration", fadeDurationMs)) {
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
		if (config_.getProperty("musicPlayer.volumeBar.fadeDelay", fadeDelayMs)) {
			// Convert from milliseconds to seconds
			volumeFadeDelay_ = static_cast<float>(fadeDelayMs) / 1000.0f;
		}
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

void MusicPlayerComponent::loadVolumeBarTextures() {
	// Get layout name from config
	std::string layoutName;
	config_.getProperty(OPTION_LAYOUT, layoutName);

	// Base paths for volume bar images
	std::vector<std::string> searchPaths;

	std::string collectionName;
	if (config_.getProperty("collection", collectionName) && !collectionName.empty()) {
		searchPaths.push_back(Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
			"collections", collectionName, "volbar"));
	}

	searchPaths.push_back(Utils::combinePath(Configuration::absolutePath, "layouts", layoutName,
		"collections", "_common", "medium_artwork", "volbar"));
	searchPaths.push_back(Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "volbar"));

	// Find empty and full images
	std::string emptyPath, fullPath;
	std::vector<std::string> extensions = { ".png", ".jpg", ".jpeg" };

	for (const auto& basePath : searchPaths) {
		for (const auto& ext : extensions) {
			std::string path = Utils::combinePath(basePath, "empty" + ext);
			if (std::filesystem::exists(path)) {
				emptyPath = path;
				break;
			}
		}
		for (const auto& ext : extensions) {
			std::string path = Utils::combinePath(basePath, "full" + ext);
			if (std::filesystem::exists(path)) {
				fullPath = path;
				break;
			}
		}
		if (!emptyPath.empty() && !fullPath.empty()) break;
	}

	if (emptyPath.empty() || fullPath.empty()) {
		LOG_ERROR("MusicPlayerComponent", "Could not find empty.png and full.png for volume bar");
		return;
	}

	// Load empty texture directly
	volumeEmptyTexture_ = IMG_LoadTexture(renderer_, emptyPath.c_str());

	SDL_Surface* fullSurfaceRaw = IMG_Load(fullPath.c_str());
	if (!fullSurfaceRaw || !volumeEmptyTexture_) {
		LOG_ERROR("MusicPlayerComponent", "Failed to load volume bar assets");
		if (fullSurfaceRaw) SDL_FreeSurface(fullSurfaceRaw);
		if (volumeEmptyTexture_) {
			SDL_DestroyTexture(volumeEmptyTexture_);
			volumeEmptyTexture_ = nullptr;
		}
		return;
	}

	// Convert to 32-bit RGBA format
	SDL_Surface* fullSurface = SDL_ConvertSurfaceFormat(fullSurfaceRaw, SDL_PIXELFORMAT_RGBA8888, 0);
	SDL_FreeSurface(fullSurfaceRaw); // no longer needed
	if (!fullSurface) {
		LOG_ERROR("MusicPlayerComponent", "Failed to convert full surface to RGBA8888");
		SDL_DestroyTexture(volumeEmptyTexture_);
		volumeEmptyTexture_ = nullptr;
		return;
	}


	totalSegments_ = detectSegmentsFromSurface(fullSurface);
	if (totalSegments_ > 0) {
		// As long as we detected segments and it's a reasonable number, use segmented mode
		// (Add an upper sanity limit to avoid extremely small segments)
		if (totalSegments_ <= 50) {  // Arbitrary upper limit to avoid unreasonable segment counts
			useSegmentedVolume_ = true;
			LOG_INFO("MusicPlayerComponent", "Using segmented volume bar with " + std::to_string(totalSegments_) + " segments");
		}
		else {
			LOG_WARNING("MusicPlayerComponent", "Segment count too high (" + std::to_string(totalSegments_) + "), using proportional volume bar");
			totalSegments_ = 0;
			useSegmentedVolume_ = false;
		}
	}
	else {
		LOG_INFO("MusicPlayerComponent", "No segments detected, using proportional volume bar");
		totalSegments_ = 0;
		useSegmentedVolume_ = false;
	}

	// Convert surface to texture
	volumeFullTexture_ = SDL_CreateTextureFromSurface(renderer_, fullSurface);
	volumeBarWidth_ = fullSurface->w;
	volumeBarHeight_ = fullSurface->h;
	baseViewInfo.ImageWidth = static_cast<float>(volumeBarWidth_);
	baseViewInfo.ImageHeight = static_cast<float>(volumeBarHeight_);
	SDL_FreeSurface(fullSurface);

	if (!volumeFullTexture_) {
		LOG_ERROR("MusicPlayerComponent", "Failed to create texture from full surface");
		SDL_DestroyTexture(volumeEmptyTexture_);
		volumeEmptyTexture_ = nullptr;
		return;
	}

	// Create the render target
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

	updateVolumeBarTexture();
}

int MusicPlayerComponent::detectSegmentsFromSurface(SDL_Surface* surface) {
	if (!surface || !surface->pixels) {
		LOG_ERROR("MusicPlayerComponent", "Invalid surface or pixel data");
		return 0;
	}

	int texW = surface->w;
	int texH = surface->h;

	// Ensure the surface is in a compatible format
	if (surface->format->BytesPerPixel != 4) {
		LOG_ERROR("MusicPlayerComponent", "Surface format is not 32-bit, cannot detect segments");
		return 0;
	}

	const Uint8 alphaThreshold = 50;  // Pixels with alpha below this are considered transparent

	// Lock surface if needed
	if (SDL_MUSTLOCK(surface)) {
		SDL_LockSurface(surface);
	}

	// Check multiple rows to find the one with the most likely segment pattern
	// We'll store the best result found
	int bestSegmentCount = 0;
	std::vector<int> bestSegmentStarts;

	// Sample rows at different positions throughout the image height
	// Try more rows for taller images
	int numRowsToCheck = std::min(20, texH); // Cap at 20 rows to avoid excessive processing

	for (int rowNum = 0; rowNum < numRowsToCheck; rowNum++) {
		// Sample rows at regular intervals throughout the height
		int y = (texH * rowNum) / numRowsToCheck;

		// Skip rows that are too close to the edges (may contain frame borders)
		if (y < texH * 0.1 || y > texH * 0.9) {
			continue;
		}

		// Access the row's pixel data
		Uint8* pixelData = static_cast<Uint8*>(surface->pixels);
		Uint32* row = reinterpret_cast<Uint32*>(pixelData + y * surface->pitch);

		// For this row, find segments
		std::vector<int> segmentStartXs;
		bool inSolidSegment = false;
		int currentSegmentWidth = 0;
		int lastSegmentStart = -1;

		// Scan this row for segments
		for (int x = 0; x < texW; ++x) {
			Uint32 pixel = row[x];
			Uint8 r, g, b, a;
			SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);

			bool isVisible = (a > alphaThreshold);

			// State transition: entering a solid segment
			if (isVisible && !inSolidSegment) {
				inSolidSegment = true;
				lastSegmentStart = x;
				currentSegmentWidth = 1;
			}
			// Continuing a solid segment
			else if (isVisible && inSolidSegment) {
				currentSegmentWidth++;
			}
			// State transition: exiting a solid segment
			else if (!isVisible && inSolidSegment) {
				inSolidSegment = false;

				// Only count segments of reasonable width (not single pixels)
				if (currentSegmentWidth >= 2) {
					segmentStartXs.push_back(lastSegmentStart);
				}
				currentSegmentWidth = 0;
			}
		}

		// Handle case where the image ends with a solid segment
		if (inSolidSegment && currentSegmentWidth >= 2) {
			segmentStartXs.push_back(lastSegmentStart);
		}

		// We need at least 2 segments to consider this a valid segmented bar
		if (segmentStartXs.size() < 2) {
			continue; // Skip this row, not enough segments
		}

		// Validate that segments are evenly spaced
		bool evenlySpaced = true;
		double averageSpacing = 0.0;
		std::vector<int> spacings;

		// Calculate spacings between segments
		for (size_t i = 1; i < segmentStartXs.size(); ++i) {
			int spacing = segmentStartXs[i] - segmentStartXs[i - 1];
			spacings.push_back(spacing);
			averageSpacing += spacing;
		}

		if (!spacings.empty()) {
			averageSpacing /= static_cast<double>(spacings.size());

			// Calculate standard deviation to measure consistency
			double varianceSum = 0.0;
			for (int spacing : spacings) {
				double diff = spacing - averageSpacing;
				varianceSum += diff * diff;
			}
			double stdDev = std::sqrt(varianceSum / static_cast<double>(spacings.size()));

			// If standard deviation is too high relative to average spacing,
			// segments aren't evenly spaced
			if (stdDev > (averageSpacing * 0.2)) { // Allow 20% variation
				evenlySpaced = false;
			}
		}

		// If segments are evenly spaced and we found more than before, update best result
		if (evenlySpaced && segmentStartXs.size() > static_cast<size_t>(bestSegmentCount)) {
			bestSegmentCount = static_cast<int>(segmentStartXs.size());
			bestSegmentStarts = segmentStartXs;

			// If we found a good number of segments, we can stop early
			if (bestSegmentCount >= 10) {
				LOG_INFO("MusicPlayerComponent", "Found good segment pattern at row " + std::to_string(y) +
					" with " + std::to_string(bestSegmentCount) + " segments");
				break;
			}
		}
	}

	// Unlock surface
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}

	if (bestSegmentCount > 0) {
		LOG_INFO("MusicPlayerComponent", "Detected " + std::to_string(bestSegmentCount) +
			" segments in volume bar image");
	}
	else {
		LOG_INFO("MusicPlayerComponent", "No segments detected in volume bar image");
	}

	return bestSegmentCount;
}

void MusicPlayerComponent::updateVolumeBarTexture() {
	if (!renderer_ || !volumeEmptyTexture_ || !volumeFullTexture_ || !volumeBarTexture_) {
		return;
	}

	SDL_Texture* previousTarget = SDL::getRenderTarget(baseViewInfo.Monitor);

	int volumeRaw = std::clamp(musicPlayer_->getLogicalVolume(), 0, 128);

	SDL_SetRenderTarget(renderer_, volumeBarTexture_);
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
	SDL_RenderClear(renderer_);

	if (useSegmentedVolume_ && totalSegments_ > 0) {
		int segmentWidth = volumeBarWidth_ / totalSegments_;
		int activeSegments = (volumeRaw * totalSegments_) / 128;

		for (int i = 0; i < totalSegments_; ++i) {
			SDL_Rect rect = {
				i * segmentWidth,
				0,
				segmentWidth,
				volumeBarHeight_
			};

			if (i < activeSegments) {
				SDL_RenderCopy(renderer_, volumeFullTexture_, &rect, &rect);
			}
			else {
				SDL_RenderCopy(renderer_, volumeEmptyTexture_, &rect, &rect);
			}
		}
	}
	else {
		// Fallback: proportional fill
		int visibleWidth = (volumeBarWidth_ * volumeRaw) / 128;

		if (visibleWidth > 0) {
			SDL_Rect src = { 0, 0, visibleWidth, volumeBarHeight_ };
			SDL_Rect dst = { 0, 0, visibleWidth, volumeBarHeight_ };
			SDL_RenderCopy(renderer_, volumeFullTexture_, &src, &dst);
		}

		if (visibleWidth < volumeBarWidth_) {
			SDL_Rect src = { visibleWidth, 0, volumeBarWidth_ - visibleWidth, volumeBarHeight_ };
			SDL_Rect dst = { visibleWidth, 0, volumeBarWidth_ - visibleWidth, volumeBarHeight_ };
			SDL_RenderCopy(renderer_, volumeEmptyTexture_, &src, &dst);
		}
	}

	SDL_SetRenderTarget(renderer_, previousTarget);
	lastVolumeValue_ = volumeRaw;
}

std::string_view MusicPlayerComponent::filePath() {
	if (loadedComponent_ != nullptr) {
		return loadedComponent_->filePath();
	}
	return "";
}

bool MusicPlayerComponent::update(float dt) {
	// Update refresh timer
	refreshTimer_ += dt;

	if (!musicPlayer_->hasStartedPlaying())
		return Component::update(dt);

	if (isVuMeter_) {
		// Update the VU levels
		updateVuLevels();

		// Apply decay to current levels
		for (int i = 0; i < vuBarCount_; i++) {
			// Decay the main level
			vuLevels_[i] = std::max(0.0f, vuLevels_[i] - (vuDecayRate_ * dt));

			// Decay the peak level more slowly
			if (vuPeaks_[i] > vuLevels_[i]) {
				vuPeaks_[i] = std::max(vuLevels_[i], vuPeaks_[i] - (vuPeakDecayRate_ * dt));
			}
		}

		// Flag texture needs update
		vuMeterNeedsUpdate_ = true;

		return Component::update(dt);
	}

	// Special handling for album art
	if (isAlbumArt_ && refreshTimer_ >= refreshInterval_) {
		refreshTimer_ = 0.0f;
		int currentTrackIndex = musicPlayer_->getCurrentTrackIndex();

		if (currentTrackIndex != albumArtTrackIndex_) {
			albumArtTrackIndex_ = currentTrackIndex;
			albumArtNeedsUpdate_ = true;
			lastState_ = std::to_string(currentTrackIndex);
		}
		return Component::update(dt);
	}

	if (isVolumeBar_) {
		int volumeRaw = std::clamp(musicPlayer_->getLogicalVolume(), 0, 128);
		bool buttonPressed = musicPlayer_->getButtonPressed();
		bool volumeChanged = (volumeRaw != lastVolumeValue_);

		// Always update last volume value so we have the correct value
		// for when we rebuild the texture
		if (volumeChanged) {
			lastVolumeValue_ = volumeRaw;
			volumeBarNeedsUpdate_ = true;
		}

		// Handle visibility state
		if (volumeChanged || buttonPressed) {
			// Reset visibility state for either volume changes or button presses
			volumeChanging_ = true;
			volumeStableTimer_ = 0.0f;

			if (buttonPressed) {
				musicPlayer_->setButtonPressed(false);
			}
		}
		// Only count down the timer if we're not actively changing
		else if (volumeChanging_) {
			volumeStableTimer_ += dt;

			// After delay, stop considering it "changing"
			if (volumeStableTimer_ >= volumeFadeDelay_) {
				volumeChanging_ = false;
			}
		}

		// Alpha calculation
		if (baseViewInfo.Alpha <= 0.0f) {
			// Layout says fully invisible
			targetAlpha_ = 0.0f;
			currentDisplayAlpha_ = 0.0f;
		}
		else {
			targetAlpha_ = volumeChanging_ ? baseViewInfo.Alpha : 0.0f;

			// Animate current alpha toward target
			if (currentDisplayAlpha_ != targetAlpha_) {
				float maxAlphaChange = dt * fadeSpeed_;

				if (currentDisplayAlpha_ < targetAlpha_) {
					currentDisplayAlpha_ = std::min(currentDisplayAlpha_ + maxAlphaChange, targetAlpha_);
				}
				else {
					currentDisplayAlpha_ = std::max(currentDisplayAlpha_ - maxAlphaChange, targetAlpha_);
				}
			}
		}

		return Component::update(dt);
	}

	// Determine current state
	std::string currentState;

	if (type_ == "state") {
		// Get the unified state
		auto state = musicPlayer_->getPlaybackState();
		// Convert the state to a string representation.
		switch (state) {
			case MusicPlayer::PlaybackState::NEXT:
			currentState = "next";
			break;
			case MusicPlayer::PlaybackState::PREVIOUS:
			currentState = "previous";
			break;
			case MusicPlayer::PlaybackState::PLAYING:
			currentState = "playing";
			break;
			case MusicPlayer::PlaybackState::PAUSED:
			currentState = "paused";
			break;
			default:
			currentState = "unknown";
			break;
		}

		// For NEXT/PREVIOUS, display the directional state for a set duration.
		if (state == MusicPlayer::PlaybackState::NEXT || state == MusicPlayer::PlaybackState::PREVIOUS) {
			directionDisplayTimer_ = directionDisplayDuration_;
		}
		else {
			if (directionDisplayTimer_ > 0.0f) {
				directionDisplayTimer_ -= dt;
				if (directionDisplayTimer_ <= 0.0f && musicPlayer_->getPlaybackState() != MusicPlayer::PlaybackState::PAUSED) {
					// After timer expiration, revert to playing state.
					musicPlayer_->setPlaybackState(MusicPlayer::PlaybackState::PLAYING);
					currentState = "playing";
				}
			}
		}
	}
	else if (type_ == "shuffle") {
		currentState = musicPlayer_->getShuffle() ? "on" : "off";
	}
	else if (type_ == "loop") {
		currentState = musicPlayer_->getLoop() ? "on" : "off";
	}
	else if (type_ == "time") {
		// For time, update on every refresh interval
		currentState = std::to_string(musicPlayer_->getCurrent());
	}
	else if (isProgressBar_) {
		currentState = std::to_string(musicPlayer_->getCurrent());
	}
	else {
		// For track/artist/album types, use the currently playing track
		currentState = musicPlayer_->getFormattedTrackInfo();
	}

	if ((currentState != lastState_) || (refreshTimer_ >= refreshInterval_)) {
		refreshTimer_ = 0.0f;
		lastState_ = currentState;

		Component* newComponent = reloadComponent();
		if (newComponent != nullptr && newComponent != loadedComponent_) {
			if (loadedComponent_ != nullptr) {
				loadedComponent_->freeGraphicsMemory();
				delete loadedComponent_;
			}
			loadedComponent_ = newComponent;
			loadedComponent_->allocateGraphicsMemory();
		}
	}

	// Update the loaded component
	if (loadedComponent_ != nullptr) {
		loadedComponent_->update(dt);
	}

	return Component::update(dt);
}

void MusicPlayerComponent::draw() {
	Component::draw();

	// If the overall alpha is 0, there's no need to draw any components.
	if (baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	// Update album art if needed
	if (isAlbumArt_ && albumArtNeedsUpdate_) {
		loadAlbumArt();
		albumArtNeedsUpdate_ = false;
	}

	// Update volume bar texture if needed
	if (isVolumeBar_ && volumeBarNeedsUpdate_) {
		updateVolumeBarTexture();
		volumeBarNeedsUpdate_ = false;
	}

	if (isVuMeter_) {
		drawVuMeter();
		return;
	}

	if (isAlbumArt_) {
		drawAlbumArt();
		return;
	}

	if (isVolumeBar_) {
		drawVolumeBar();
		return;
	}

	if (isProgressBar_) {
		drawProgressBar();
		return;
	}

	if (loadedComponent_ != nullptr) {
		loadedComponent_->baseViewInfo = baseViewInfo;
		loadedComponent_->draw();
	}
}


void MusicPlayerComponent::createVuMeterTextureIfNeeded() {
	if (!renderer_ || vuMeterTexture_ != nullptr) {
		return; // Already created or renderer not available
	}

	// Get the dimensions from baseViewInfo
	vuMeterTextureWidth_ = static_cast<int>(baseViewInfo.ScaledWidth());
	vuMeterTextureHeight_ = static_cast<int>(baseViewInfo.ScaledHeight());

	// Ensure we have valid dimensions
	if (vuMeterTextureWidth_ <= 0 || vuMeterTextureHeight_ <= 0) {
		return;
	}

	// Create the render target texture
	vuMeterTexture_ = SDL_CreateTexture(
		renderer_,
		SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_TARGET,
		vuMeterTextureWidth_,
		vuMeterTextureHeight_
	);

	if (vuMeterTexture_) {
		SDL_SetTextureBlendMode(vuMeterTexture_, SDL_BLENDMODE_BLEND);
		vuMeterNeedsUpdate_ = true;
	}
	else {
		LOG_ERROR("MusicPlayerComponent", "Failed to create VU meter texture");
	}
}

// Add new method to update the VU meter texture
void MusicPlayerComponent::updateVuMeterTexture() {
	if (!renderer_ || !vuMeterTexture_ || !vuMeterNeedsUpdate_) {
		return;
	}

	// Save current render target
	SDL_Texture* previousTarget = SDL::getRenderTarget(baseViewInfo.Monitor);

	// Set the VU meter texture as the render target
	SDL_SetRenderTarget(renderer_, vuMeterTexture_);

	// Clear the texture
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
	SDL_RenderClear(renderer_);

	// Calculate bar dimensions
	float barWidth = static_cast<float>(vuMeterTextureWidth_) / static_cast<float>(vuBarCount_);
	float barSpacing = barWidth * 0.1f; // 10% of bar width for spacing
	float actualBarWidth = barWidth - barSpacing;

	// Set blend mode for transparency
	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

	// Draw each bar to the texture
	for (int i = 0; i < vuBarCount_; i++) {
		float barX = static_cast<float>(i * barWidth);

		// Calculate the height of this bar based on its level
		float barHeight = static_cast<float>(vuMeterTextureHeight_) * vuLevels_[i];
		float peakHeight = static_cast<float>(vuMeterTextureHeight_) * vuPeaks_[i];

		// Background/border for bar
		SDL_SetRenderDrawColor(
			renderer_,
			vuBackgroundColor_.r,
			vuBackgroundColor_.g,
			vuBackgroundColor_.b,
			255 // Full opacity on the texture
		);
		SDL_FRect bgRect = { barX, 0, actualBarWidth, static_cast<float>(vuMeterTextureHeight_) };
		SDL_RenderFillRectF(renderer_, &bgRect);

		// Calculate zone heights based on thresholds
		float greenZone = vuMeterTextureHeight_ * vuGreenThreshold_;
		float yellowZone = vuMeterTextureHeight_ * (vuYellowThreshold_ - vuGreenThreshold_);
		float redZone = vuMeterTextureHeight_ * (1.0f - vuYellowThreshold_);

		// Draw the green segment
		if (barHeight > 0) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuBottomColor_.r,
				vuBottomColor_.g,
				vuBottomColor_.b,
				255 // Full opacity on the texture
			);
			float segmentHeight = std::min(barHeight, greenZone);
			SDL_FRect greenRect = {
				barX + barSpacing * 0.5f,
				vuMeterTextureHeight_ - segmentHeight,
				actualBarWidth - barSpacing,
				segmentHeight
			};
			SDL_RenderFillRectF(renderer_, &greenRect);
		}

		// Draw the yellow segment
		if (barHeight > greenZone) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuMiddleColor_.r,
				vuMiddleColor_.g,
				vuMiddleColor_.b,
				255 // Full opacity on the texture
			);
			float segmentHeight = std::min(barHeight - greenZone, yellowZone);
			SDL_FRect yellowRect = {
				barX + barSpacing * 0.5f,
				vuMeterTextureHeight_ - greenZone - segmentHeight,
				actualBarWidth - barSpacing,
				segmentHeight
			};
			SDL_RenderFillRectF(renderer_, &yellowRect);
		}

		// Draw the red segment
		if (barHeight > greenZone + yellowZone) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuTopColor_.r,
				vuTopColor_.g,
				vuTopColor_.b,
				255 // Full opacity on the texture
			);
			float segmentHeight = std::min(barHeight - greenZone - yellowZone, redZone);
			SDL_FRect redRect = {
				barX + barSpacing * 0.5f,
				vuMeterTextureHeight_ - greenZone - yellowZone - segmentHeight,
				actualBarWidth - barSpacing,
				segmentHeight
			};
			SDL_RenderFillRectF(renderer_, &redRect);
		}

		// Draw peak marker
		if (peakHeight > 0 && peakHeight >= barHeight) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuPeakColor_.r,
				vuPeakColor_.g,
				vuPeakColor_.b,
				255 // Full opacity on the texture
			);

			SDL_FRect peakRect = {
				barX + barSpacing * 0.5f,
				vuMeterTextureHeight_ - peakHeight - 2,
				actualBarWidth - barSpacing,
				2 // 2-pixel height for the peak marker
			};
			SDL_RenderFillRectF(renderer_, &peakRect);
		}
	}

	// Restore previous render target
	SDL_SetRenderTarget(renderer_, previousTarget);

	// Mark as updated
	vuMeterNeedsUpdate_ = false;
}


bool MusicPlayerComponent::parseHexColor(const std::string& hexString, SDL_Color& outColor) {
	std::string_view sv = hexString;

	// Must be exactly 6 characters long
	if (sv.length() != 6) {
		LOG_WARNING("MusicPlayerComponent", "Invalid length for hex string: " + hexString);
		return false;
	}

	// Check if all characters are hex digits
	for (char c : sv) {
		if (!std::isxdigit(static_cast<unsigned char>(c))) {
			LOG_WARNING("parseHexColor", "Non-hex character found in string: " + hexString);
			return false;
		}
	}

	// Use std::from_chars for safe conversion (C++17)
	unsigned int r, g, b;
	// Using .data() and .data() + length is correct for string_view with from_chars
	auto result_r = std::from_chars(sv.data(), sv.data() + 2, r, 16);
	auto result_g = std::from_chars(sv.data() + 2, sv.data() + 4, g, 16);
	auto result_b = std::from_chars(sv.data() + 4, sv.data() + 6, b, 16);

	// Check if parsing succeeded for all components
	if (result_r.ec != std::errc() || result_g.ec != std::errc() || result_b.ec != std::errc() ||
		result_r.ptr != sv.data() + 2 || // Ensure exactly 2 chars were consumed for R
		result_g.ptr != sv.data() + 4 || // Ensure exactly 2 chars were consumed for G
		result_b.ptr != sv.data() + 6)   // Ensure exactly 2 chars were consumed for B
	{
		LOG_WARNING("MusicPlayerComponent", "Hex conversion failed for string: " + hexString); // Optional debug log
		return false;
	}


	// Assign to SDL_Color (values are already guaranteed 0-255 by hex format and successful parse)
	outColor.r = static_cast<Uint8>(r);
	outColor.g = static_cast<Uint8>(g);
	outColor.b = static_cast<Uint8>(b);
	outColor.a = 255; // Full opacity

	return true;
}

void MusicPlayerComponent::updateVuLevels() {
	// Get audio levels from the music player
	const std::vector<float>& audioLevels = musicPlayer_->getAudioLevels();
	int channels = musicPlayer_->getAudioChannels(); // Still potentially useful

	if (!musicPlayer_->isPlaying() || audioLevels.empty()) {
		// If not playing, rapidly reduce all levels
		for (auto& level : vuLevels_) {
			level *= 0.8f; // Faster decay when not playing
		}
		for (auto& peak : vuPeaks_) {
			peak *= 0.9f;
		}
		return;
	}

	// Amplification factor
	const float amplification = 5.0f;

	// --- MONO / STEREO SPLIT ---
	if (vuMeterIsMono_) {
		// --- MONO MODE ---
		// Calculate average level across all available channels
		float averageLevel = 0.0f;
		float sum = 0.0f;
		for (float level : audioLevels) {
			sum += level;
		}
		if (!audioLevels.empty()) {
			averageLevel = sum / static_cast<float>(audioLevels.size());
		}

		// Amplify and clamp the average level
		float monoLevel = std::min(1.0f, averageLevel * amplification);

		// Apply this monoLevel to all bars using the existing mono pattern logic
		for (int i = 0; i < vuBarCount_; i++) {
			// Create a pattern (reusing logic from original mono implementation)
			float barPos = static_cast<float>(i) / vuBarCount_;
			float patternFactor;
			if (i % 2 == 0) {
				patternFactor = 1.0f - std::abs(barPos - 0.5f) * 0.6f;
			}
			else {
				patternFactor = 1.0f + 0.3f * std::sin(barPos * 3.14159f * 4);
			}
			float randomFactor = 1.0f + ((rand() % 25) - 10) / 100.0f;
			float newLevel = monoLevel * patternFactor * randomFactor;
			newLevel = std::min(1.0f, std::pow(newLevel, 0.75f));

			if (newLevel > vuLevels_[i]) {
				vuLevels_[i] = newLevel;
				vuPeaks_[i] = std::max(vuPeaks_[i], newLevel);
			}
		}
	}
	else {
		// --- STEREO MODE (or fallback if not explicitly mono) ---
		if (channels >= 2) {
			// Stereo mode: left channel for left half, right channel for right half
			int leftBars = vuBarCount_ / 2;
			int rightBars = vuBarCount_ - leftBars;

			// Left channel (first half of bars)
			float leftLevel = std::min(1.0f, audioLevels[0] * amplification);
			for (int i = 0; i < leftBars; i++) {
				float barFactor = 1.0f - (static_cast<float>(i) / leftBars) * 0.5f;
				float randomFactor = 1.0f + ((rand() % 20) - 10) / 100.0f;
				float newLevel = leftLevel * barFactor * randomFactor;
				newLevel = std::min(1.0f, std::pow(newLevel, 0.8f));
				if (newLevel > vuLevels_[i]) {
					vuLevels_[i] = newLevel;
					vuPeaks_[i] = std::max(vuPeaks_[i], newLevel);
				}
			}

			// Right channel (second half of bars)
			float rightLevel = std::min(1.0f, audioLevels[1] * amplification);
			for (int i = 0; i < rightBars; i++) {
				int barIndex = leftBars + i;
				float barFactor = 1.0f - (static_cast<float>(i) / rightBars) * 0.5f;
				float randomFactor = 1.0f + ((rand() % 20) - 10) / 100.0f;
				float newLevel = rightLevel * barFactor * randomFactor;
				newLevel = std::min(1.0f, std::pow(newLevel, 0.8f));
				if (newLevel > vuLevels_[barIndex]) {
					vuLevels_[barIndex] = newLevel;
					vuPeaks_[barIndex] = std::max(vuPeaks_[barIndex], newLevel);
				}
			}

			// Add extra dynamics: occasionally boost random bars
			if ((rand() % 10) < 3) { // 30% chance each update
				int barToBoost = rand() % vuBarCount_;
				vuLevels_[barToBoost] = std::min(1.0f, vuLevels_[barToBoost] * 1.3f);
				vuPeaks_[barToBoost] = std::max(vuPeaks_[barToBoost], vuLevels_[barToBoost]);
			}
		}
		else {
			// Fallback for single channel audio when not in explicit mono mode
			// Treat the single channel as mono input
			float level = std::min(1.0f, audioLevels[0] * amplification);
			for (int i = 0; i < vuBarCount_; i++) {
				// Simple distribution: apply level directly, maybe with slight variation
				float randomFactor = 1.0f + ((rand() % 10) - 5) / 100.0f; // +/- 5%
				float newLevel = std::min(1.0f, level * randomFactor);

				if (newLevel > vuLevels_[i]) {
					vuLevels_[i] = newLevel;
					vuPeaks_[i] = std::max(vuPeaks_[i], newLevel);
				}
			}
		}
	}
}

void MusicPlayerComponent::drawVuMeter() {
	if (!renderer_ || !musicPlayer_->hasStartedPlaying()) {
		return;
	}

	// Create texture if needed
	createVuMeterTextureIfNeeded();

	// If texture creation failed, return
	if (!vuMeterTexture_) {
		return;
	}

	// Update the texture if needed
	if (vuMeterNeedsUpdate_) {
		updateVuMeterTexture();
	}

	// Draw the texture
	SDL_FRect rect;
	rect.x = baseViewInfo.XRelativeToOrigin();
	rect.y = baseViewInfo.YRelativeToOrigin();
	rect.w = baseViewInfo.ScaledWidth();
	rect.h = baseViewInfo.ScaledHeight();

	SDL::renderCopyF(
		vuMeterTexture_,
		baseViewInfo.Alpha,
		nullptr,
		&rect,
		baseViewInfo,
		page.getLayoutWidth(baseViewInfo.Monitor),
		page.getLayoutHeight(baseViewInfo.Monitor)
	);
}

void MusicPlayerComponent::drawProgressBar() {
	if (!renderer_) {
		return;
	}

	// Get current track progress
	float current = static_cast<float>(musicPlayer_->getCurrent());
	float duration = static_cast<float>(musicPlayer_->getDuration());

	if (duration <= 0) {
		return; // Avoid division by zero
	}

	float progressPercent = current / duration;

	// Use layout-defined dimensions
	float barX = baseViewInfo.XRelativeToOrigin();
	float barY = baseViewInfo.YRelativeToOrigin();
	float barWidth = baseViewInfo.ScaledWidth();  // Full width from layout
	float barHeight = baseViewInfo.ScaledHeight(); // Height from layout

	float filledWidth = barWidth * progressPercent;
	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

	// Set the background bar color (black)
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, static_cast<Uint8>(baseViewInfo.Alpha * 255));

	// Draw the full background bar
	SDL_FRect backgroundRect = { barX, barY, barWidth, barHeight };
	SDL_RenderFillRectF(renderer_, &backgroundRect);

	// Set the progress bar color (white)
	SDL_SetRenderDrawColor(renderer_, 255, 255, 255, static_cast<Uint8>(baseViewInfo.Alpha * 255));

	// Draw the filled portion (progress)
	SDL_FRect progressRect = { barX, barY, filledWidth, barHeight };
	SDL_RenderFillRectF(renderer_, &progressRect);
}



void MusicPlayerComponent::drawAlbumArt() {
	if (!renderer_) {
		return;
	}

	// Since update(dt) is responsible for loading, simply render if the texture exists.
	if (albumArtTexture_ != nullptr) {
		SDL_FRect rect;
		rect.x = baseViewInfo.XRelativeToOrigin();
		rect.y = baseViewInfo.YRelativeToOrigin();
		rect.w = baseViewInfo.ScaledWidth();
		rect.h = baseViewInfo.ScaledHeight();
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

void MusicPlayerComponent::loadAlbumArt() {
	if (albumArtTexture_ != nullptr) {
		SDL_DestroyTexture(albumArtTexture_);
		albumArtTexture_ = nullptr;
	}
	// Try to get album art from the music player
	std::vector<unsigned char> albumArtData;
	if (musicPlayer_->getAlbumArt(albumArtTrackIndex_, albumArtData) && !albumArtData.empty()) {
		SDL_RWops* rw = SDL_RWFromConstMem(albumArtData.data(), static_cast<int>(albumArtData.size()));
		if (rw) {
			albumArtTexture_ = IMG_LoadTexture_RW(renderer_, rw, 1); // 1 means auto-close
			if (albumArtTexture_) {
				SDL_QueryTexture(albumArtTexture_, nullptr, nullptr,
					&albumArtTextureWidth_, &albumArtTextureHeight_);
				baseViewInfo.ImageWidth = static_cast<float>(albumArtTextureWidth_);
				baseViewInfo.ImageHeight = static_cast<float>(albumArtTextureHeight_);
				LOG_INFO("MusicPlayerComponent", "Created album art texture");
				return;
			}
		}
	}
	// Fallback: load default album art if none found or on error
	albumArtTexture_ = loadDefaultAlbumArt();
}

SDL_Texture* MusicPlayerComponent::loadDefaultAlbumArt() {
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
				baseViewInfo.ImageWidth = static_cast<float>(albumArtTextureWidth_);
				baseViewInfo.ImageHeight = static_cast<float>(albumArtTextureHeight_);
				LOG_INFO("MusicPlayerComponent", "Loaded default album art from: " + path);
				return texture;
			}
		}
	}

	LOG_WARNING("MusicPlayerComponent", "Failed to load default album art");
	return nullptr;
}

void MusicPlayerComponent::drawVolumeBar() {
	if (!renderer_ || !volumeBarTexture_) {
		return;
	}

	// Draw the volume bar texture
	SDL_FRect rect;

	// Use the baseViewInfo for position and size calculations
	rect.x = baseViewInfo.XRelativeToOrigin();
	rect.y = baseViewInfo.YRelativeToOrigin();
	rect.w = baseViewInfo.ScaledWidth();
	rect.h = baseViewInfo.ScaledHeight();

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

Component* MusicPlayerComponent::reloadComponent() {
	// Album art is handled directly, don't create a component for it
	if (isAlbumArt_ || isVolumeBar_ || !musicPlayer_->hasStartedPlaying()) {
		return nullptr;
	}

	Component* component = nullptr;
	std::string typeLC = Utils::toLower(type_);
	std::string basename;

	// Determine the basename based on component type
	if (typeLC == "state") {
		// Use the unified PlaybackState from MusicPlayer.
		MusicPlayer::PlaybackState state = musicPlayer_->getPlaybackState();

		// If we have a directional state (NEXT or PREVIOUS) and fading is done,
		// reset the state to PLAYING if music is playing.
		if ((state == MusicPlayer::PlaybackState::NEXT || state == MusicPlayer::PlaybackState::PREVIOUS) &&
			!musicPlayer_->isFading()) {
			if (musicPlayer_->isPlaying()) {
				musicPlayer_->setPlaybackState(MusicPlayer::PlaybackState::PLAYING);
			}
		}
		// Update our local copy of the state.
		state = musicPlayer_->getPlaybackState();

		// Set basename based on the unified state.
		if (state == MusicPlayer::PlaybackState::NEXT) {
			basename = "next";
		}
		else if (state == MusicPlayer::PlaybackState::PREVIOUS) {
			basename = "previous";
		}
		else if (state == MusicPlayer::PlaybackState::PLAYING) {
			basename = "playing";
		}
		else if (state == MusicPlayer::PlaybackState::PAUSED) {
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

		if (loadedComponent_) {
			loadedComponent_->setText(fileName);
		}
		else {
			loadedComponent_ = new Text(fileName, page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
	}
	else if (typeLC == "trackinfo") {
		std::string trackName = musicPlayer_->getFormattedTrackInfo();
		if (trackName.empty()) {
			trackName = "No track playing";
		}

		if (loadedComponent_) {
			loadedComponent_->setText(trackName);
		}
		else {
			loadedComponent_ = new Text(trackName, page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
	}
	else if (typeLC == "title") {
		std::string titleName = musicPlayer_->getCurrentTitle();
		if (titleName.empty()) {
			titleName = "Unknown";
		}
		if (loadedComponent_) {
			loadedComponent_->setText(titleName);
		}
		else {
			loadedComponent_ = new Text(titleName, page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
	}
	else if (typeLC == "artist") {
		std::string artistName = musicPlayer_->getCurrentArtist();
		if (artistName.empty()) {
			artistName = "Unknown Artist";
		}

		if (loadedComponent_) {
			loadedComponent_->setText(artistName);
		}
		else {
			loadedComponent_ = new Text(artistName, page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
	}
	else if (typeLC == "album") {
		std::string albumName = musicPlayer_->getCurrentAlbum();
		if (albumName.empty()) {
			albumName = "Unknown Album";
		}

		if (loadedComponent_) {
			loadedComponent_->setText(albumName);
		}
		else {
			loadedComponent_ = new Text(albumName, page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
	}
	else if (typeLC == "time") {
		auto [currentSec, durationSec] = musicPlayer_->getCurrentAndDurationSec();

		if (currentSec < 0)
			return nullptr;

		int currentMin = currentSec / 60;
		int currentRemSec = currentSec % 60;
		int durationMin = durationSec / 60;
		int durationRemSec = durationSec % 60;

		std::stringstream ss;
		int minWidth = durationMin >= 10 ? 2 : 1;

		ss << std::setfill('0') << std::setw(minWidth) << currentMin << ":"
			<< std::setfill('0') << std::setw(2) << currentRemSec << "/"
			<< std::setfill('0') << std::setw(minWidth) << durationMin << ":"
			<< std::setfill('0') << std::setw(2) << durationRemSec;

		if (loadedComponent_) {
			loadedComponent_->setText(ss.str());
		}
		else {
			loadedComponent_ = new Text(ss.str(), page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
	}
	else if (typeLC == "volume") {
		int volumeRaw = musicPlayer_->getVolume();
		int volumePercentage = static_cast<int>((volumeRaw / 128.0f) * 100.0f + 0.5f);
		std::string volumeStr = std::to_string(volumePercentage);

		if (loadedComponent_) {
			loadedComponent_->setText(volumeStr);
		}
		else {
			loadedComponent_ = new Text(volumeStr, page, font_, baseViewInfo.Monitor);
		}
		return loadedComponent_;
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

void MusicPlayerComponent::pause() {
	if (musicPlayer_->isPlaying()) {
		musicPlayer_->pauseMusic();
	}
	else {
		musicPlayer_->playMusic();
	}
}

unsigned long long MusicPlayerComponent::getCurrent() {
	return static_cast<unsigned long long>(musicPlayer_->getCurrent());
}

unsigned long long MusicPlayerComponent::getDuration() {
	return static_cast<unsigned long long>(musicPlayer_->getDuration());
}

bool MusicPlayerComponent::isPaused() {
	return musicPlayer_->isPaused();
}

bool MusicPlayerComponent::isPlaying() {
	return musicPlayer_->isPlaying();
}