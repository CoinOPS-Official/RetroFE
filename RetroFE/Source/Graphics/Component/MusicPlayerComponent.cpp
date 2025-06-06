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
	, progressBarTexture_(nullptr)
	, progressBarTextureWidth_(0)
	, progressBarTextureHeight_(0)
	, progressBarNeedsUpdate_{ true } // Start with needing an update
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

namespace ImageProcessorConstants {
	const float SCAN_AREA_TOP_RATIO = 0.3f;
	const float SCAN_AREA_BOTTOM_RATIO = 0.9f;
	const Uint8 ALPHA_THRESHOLD = 50;
	const float LUMINANCE_JUMP_THRESHOLD = 50.0f;
	const int MIN_SEGMENT_WIDTH = 2;
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

	if (progressBarTexture_ != nullptr) {
		SDL_DestroyTexture(progressBarTexture_);
		progressBarTexture_ = nullptr;
	}

	if (loadedComponent_ != nullptr) {
		loadedComponent_->freeGraphicsMemory();
		delete loadedComponent_;
		loadedComponent_ = nullptr;
	}
}

void MusicPlayerComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();

	// Get the renderer if we're going to handle album art or volume bar or progress bar or VU meter
	if (isAlbumArt_ || isVolumeBar_ || isProgressBar_ || isVuMeter_) {
		renderer_ = SDL::getRenderer(baseViewInfo.Monitor);
	}

	if (isVuMeter_) {
		musicPlayer_->registerVisualizerCallback();

		config_.getProperty("musicPlayer.vuMeter.mono", vuMeterIsMono_);
		if (vuMeterIsMono_) {
			LOG_INFO("MusicPlayerComponent", "VU Meter configured for mono display.");
		}
		else {
			LOG_INFO("MusicPlayerComponent", "VU Meter configured for stereo display (default).");
		}

		int configBarCount;
		if (config_.getProperty("musicPlayer.vuMeter.barCount", configBarCount)) {
			vuBarCount_ = std::max(1, std::min(32, configBarCount));
		}

		float configDecayRate;
		if (config_.getProperty("musicPlayer.vuMeter.decayRate", configDecayRate)) {
			vuDecayRate_ = std::max(0.1f, configDecayRate);
		}

		float configPeakDecayRate;
		if (config_.getProperty("musicPlayer.vuMeter.peakDecayRate", configPeakDecayRate)) {
			vuPeakDecayRate_ = std::max(0.1f, configPeakDecayRate);
		}

		std::string colorStr;
		SDL_Color parsedColor;

		if (config_.getProperty("musicPlayer.vuMeter.bottomColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuBottomColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.bottomColor: '" + colorStr + "'. Using default.");
			}
		}

		if (config_.getProperty("musicPlayer.vuMeter.middleColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuMiddleColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.middleColor: '" + colorStr + "'. Using default.");
			}
		}

		if (config_.getProperty("musicPlayer.vuMeter.topColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuTopColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.topColor: '" + colorStr + "'. Using default.");
			}
		}

		if (config_.getProperty("musicPlayer.vuMeter.backgroundColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuBackgroundColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.backgroundColor: '" + colorStr + "'. Using default.");
			}
		}

		if (config_.getProperty("musicPlayer.vuMeter.peakColor", colorStr)) {
			if (parseHexColor(colorStr, parsedColor)) {
				vuPeakColor_ = parsedColor;
			}
			else {
				LOG_WARNING("MusicPlayerComponent", "Invalid format for musicPlayer.vuMeter.peakColor: '" + colorStr + "'. Using default.");
			}
		}

		float threshold;
		if (config_.getProperty("musicPlayer.vuMeter.greenThreshold", threshold)) {
			vuGreenThreshold_ = std::max(0.0f, std::min(1.0f, threshold));
		}

		if (config_.getProperty("musicPlayer.vuMeter.yellowThreshold", threshold)) {
			vuYellowThreshold_ = std::max(0.0f, std::min(1.0f, threshold));
			vuYellowThreshold_ = std::max(vuYellowThreshold_, vuGreenThreshold_);
		}

		vuLevels_.resize(vuBarCount_, 0.0f);
		vuPeaks_.resize(vuBarCount_, 0.0f);
	}


	if (isVolumeBar_) {
		loadVolumeBarTextures();
		int fadeDurationMs = 333;
		if (config_.getProperty("musicPlayer.volumeBar.fadeDuration", fadeDurationMs)) {
			fadeDurationMs = std::max(1, fadeDurationMs);
			float fadeDurationSeconds = static_cast<float>(fadeDurationMs) / 1000.0f;
			fadeSpeed_ = 1.0f / fadeDurationSeconds;
			LOG_INFO("MusicPlayerComponent",
				"Volume bar fade duration set to " + std::to_string(fadeDurationMs) + "ms");
		}
		int fadeDelayMs = 1500;
		if (config_.getProperty("musicPlayer.volumeBar.fadeDelay", fadeDelayMs)) {
			volumeFadeDelay_ = static_cast<float>(fadeDelayMs) / 1000.0f;
		}
	}
	// Only create loadedComponent if this isn't a special type we handle directly
	// (album art, volume bar, progress bar, vu meter)
	else if (!isAlbumArt_ && !isVolumeBar_ && !isProgressBar_ && !isVuMeter_) {
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
		if (totalSegments_ <= 50) {
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
	if (!surface || !surface->pixels) return 0;
	if (surface->format->BytesPerPixel != 4) return 0;

	const int texW = surface->w;
	const int texH = surface->h;
	const auto scanYStart = static_cast<int>(texH * ImageProcessorConstants::SCAN_AREA_TOP_RATIO);
	const auto scanYEnd = static_cast<int>(texH * ImageProcessorConstants::SCAN_AREA_BOTTOM_RATIO);

	std::map<int, int> segmentCountHistogram;

	if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);

	for (int y = scanYStart; y < scanYEnd; ++y) {
		Uint8* pixelData = static_cast<Uint8*>(surface->pixels);
		Uint32* row = reinterpret_cast<Uint32*>(pixelData + y * surface->pitch);

		std::vector<int> segmentStartXs;
		bool inSegment = false;
		int segmentStart = -1;

		float previousLuminance = 0.0f;

		for (int x = 0; x < texW; ++x) {
			Uint32 pixel = row[x];
			Uint8 r, g, b, a;
			SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);

			float currentLuminance = 0.0f;
			if (a > ImageProcessorConstants::ALPHA_THRESHOLD) {
				currentLuminance = (0.299f * r) + (0.587f * g) + (0.114f * b);
			}

			float luminanceChange = currentLuminance - previousLuminance;

			// RISING EDGE: Did we transition from dark to bright?
			if (!inSegment && luminanceChange > ImageProcessorConstants::LUMINANCE_JUMP_THRESHOLD) {
				inSegment = true;
				segmentStart = x;
			}
			// FALLING EDGE: Did we transition from bright to dark?
			else if (inSegment && luminanceChange < -ImageProcessorConstants::LUMINANCE_JUMP_THRESHOLD) {
				inSegment = false;
				if (x - segmentStart >= ImageProcessorConstants::MIN_SEGMENT_WIDTH) {
					segmentStartXs.push_back(segmentStart);
				}
			}

			previousLuminance = currentLuminance;
		}

		// Handle case where a segment runs to the very edge of the image
		if (inSegment && texW - segmentStart >= ImageProcessorConstants::MIN_SEGMENT_WIDTH) {
			segmentStartXs.push_back(segmentStart);
		}

		if (!segmentStartXs.empty()) {
			segmentCountHistogram[segmentStartXs.size()]++;
		}
	}

	if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);

	if (segmentCountHistogram.empty()) {
		return 0;
	}

	if (!segmentCountHistogram.empty()) {
		auto mostCommonEntry = std::max_element(
			segmentCountHistogram.begin(),
			segmentCountHistogram.end(),
			[](const auto& a, const auto& b) { return a.second < b.second; }
		);
		LOG_INFO("MusicPlayerComponent", "Detected " + std::to_string(mostCommonEntry->first) +
			" segments in volume bar image (modal scanline count)");
		return mostCommonEntry->first;
	}
	else {
		LOG_INFO("MusicPlayerComponent", "No segments detected in volume bar image");
		return 0;
	}
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
	refreshTimer_ += dt;

	if (!musicPlayer_->hasStartedPlaying())
		return Component::update(dt);

	if (isVuMeter_) {
		updateVuLevels();
		for (int i = 0; i < vuBarCount_; i++) {
			vuLevels_[i] = std::max(0.0f, vuLevels_[i] - (vuDecayRate_ * dt));
			if (vuPeaks_[i] > vuLevels_[i]) {
				vuPeaks_[i] = std::max(vuLevels_[i], vuPeaks_[i] - (vuPeakDecayRate_ * dt));
			}
		}
		vuMeterNeedsUpdate_ = true;
		return Component::update(dt);
	}

	if (isAlbumArt_) {
		if (refreshTimer_ >= refreshInterval_) {
			refreshTimer_ = 0.0f;
			int currentTrackIndex = musicPlayer_->getCurrentTrackIndex();
			if (currentTrackIndex != albumArtTrackIndex_) {
				albumArtTrackIndex_ = currentTrackIndex;
				albumArtNeedsUpdate_ = true;
				// lastState_ = std::to_string(currentTrackIndex); // This was incorrect for album art
			}
		}
		return Component::update(dt);
	}

	if (isVolumeBar_) {
		int volumeRaw = std::clamp(musicPlayer_->getLogicalVolume(), 0, 128);
		bool buttonPressed = musicPlayer_->getButtonPressed();
		bool volumeChanged = (volumeRaw != lastVolumeValue_);

		if (volumeChanged) {
			lastVolumeValue_ = volumeRaw;
			volumeBarNeedsUpdate_ = true;
		}

		if (volumeChanged || buttonPressed) {
			volumeChanging_ = true;
			volumeStableTimer_ = 0.0f;
			if (buttonPressed) {
				musicPlayer_->setButtonPressed(false);
			}
		}
		else if (volumeChanging_) {
			volumeStableTimer_ += dt;
			if (volumeStableTimer_ >= volumeFadeDelay_) {
				volumeChanging_ = false;
			}
		}

		if (baseViewInfo.Alpha <= 0.0f) {
			targetAlpha_ = 0.0f;
			currentDisplayAlpha_ = 0.0f;
		}
		else {
			targetAlpha_ = volumeChanging_ ? baseViewInfo.Alpha : 0.0f;
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

	if (isProgressBar_) {
		if (refreshTimer_ >= refreshInterval_) {
			refreshTimer_ = 0.0f;
			float current = static_cast<float>(musicPlayer_->getCurrent());
			float duration = static_cast<float>(musicPlayer_->getDuration());
			float currentProgressPercent = (duration > 0.0f) ? (current / duration) : 0.0f;

			// Check for significant change or if texture not yet created/valid
			if (progressBarTexture_ == nullptr ||
				progressBarTextureWidth_ <= 0 || progressBarTextureHeight_ <= 0 || // Ensure texture is valid
				std::abs(currentProgressPercent - lastProgressPercent_) > 0.001f) { // 0.1% change
				progressBarNeedsUpdate_ = true;
			}
		}
		return Component::update(dt);
	}


	// If none of the above, it's a type that uses loadedComponent_
	std::string currentState;
	if (type_ == "state") {
		auto state = musicPlayer_->getPlaybackState();
		switch (state) {
			case MusicPlayer::PlaybackState::NEXT: currentState = "next"; break;
			case MusicPlayer::PlaybackState::PREVIOUS: currentState = "previous"; break;
			case MusicPlayer::PlaybackState::PLAYING: currentState = "playing"; break;
			case MusicPlayer::PlaybackState::PAUSED: currentState = "paused"; break;
			default: currentState = "unknown"; break;
		}
		if (state == MusicPlayer::PlaybackState::NEXT || state == MusicPlayer::PlaybackState::PREVIOUS) {
			directionDisplayTimer_ = directionDisplayDuration_;
		}
		else {
			if (directionDisplayTimer_ > 0.0f) {
				directionDisplayTimer_ -= dt;
				if (directionDisplayTimer_ <= 0.0f && musicPlayer_->getPlaybackState() != MusicPlayer::PlaybackState::PAUSED) {
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
		currentState = std::to_string(musicPlayer_->getCurrent());
	}
	// Note: isProgressBar_ is handled above and returns. If type_ == "progress" was meant for text,
	// it would need a different type string or logic here.
	else { // For track/artist/album/filename types
		currentState = musicPlayer_->getFormattedTrackInfo(); // Default, specific logic in reloadComponent
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

	if (loadedComponent_ != nullptr && baseViewInfo.Alpha > 0.0f) { // Only update if visible
		loadedComponent_->update(dt);
	}

	return Component::update(dt);
}

void MusicPlayerComponent::draw() {
	Component::draw();

	if (baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	if (isAlbumArt_ && albumArtNeedsUpdate_) {
		loadAlbumArt();
		albumArtNeedsUpdate_ = false;
	}

	if (isVolumeBar_ && volumeBarNeedsUpdate_) {
		updateVolumeBarTexture();
		volumeBarNeedsUpdate_ = false;
	}

	if (isVuMeter_) {
		createVuMeterTextureIfNeeded();
		if (vuMeterTexture_ && vuMeterNeedsUpdate_) {
			updateVuMeterTexture(); // vuMeterNeedsUpdate_ is set in update()
		}
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
		createProgressBarTextureIfNeeded();
		if (progressBarTexture_ && progressBarNeedsUpdate_) {
			updateProgressBarTexture(); // progressBarNeedsUpdate_ is set in update()
		}
		drawProgressBarTexture(); // Use the new method to draw the texture
		return;
	}

	if (loadedComponent_ != nullptr) {
		loadedComponent_->baseViewInfo = baseViewInfo;
		loadedComponent_->draw();
	}
}


void MusicPlayerComponent::createVuMeterTextureIfNeeded() {
	if (!renderer_) return;

	int targetWidth = static_cast<int>(baseViewInfo.ScaledWidth());
	int targetHeight = static_cast<int>(baseViewInfo.ScaledHeight());

	if (targetWidth <= 0 || targetHeight <= 0) {
		if (vuMeterTexture_ != nullptr) {
			SDL_DestroyTexture(vuMeterTexture_);
			vuMeterTexture_ = nullptr;
			vuMeterTextureWidth_ = 0;
			vuMeterTextureHeight_ = 0;
		}
		return;
	}

	if (vuMeterTexture_ == nullptr || vuMeterTextureWidth_ != targetWidth || vuMeterTextureHeight_ != targetHeight) {
		if (vuMeterTexture_ != nullptr) {
			SDL_DestroyTexture(vuMeterTexture_);
			vuMeterTexture_ = nullptr;
		}
		vuMeterTextureWidth_ = targetWidth;
		vuMeterTextureHeight_ = targetHeight;

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
			LOG_INFO("MusicPlayerComponent", "Created/Resized VU Meter texture: " +
				std::to_string(vuMeterTextureWidth_) + "x" +
				std::to_string(vuMeterTextureHeight_));
		}
		else {
			LOG_ERROR("MusicPlayerComponent", "Failed to create VU meter texture");
			vuMeterTextureWidth_ = 0;
			vuMeterTextureHeight_ = 0;
		}
	}
}

void MusicPlayerComponent::updateVuMeterTexture() {
	if (!renderer_ || !vuMeterTexture_) { // Removed !vuMeterNeedsUpdate_ check, as it's called when needed
		return;
	}

	SDL_Texture* previousTarget = SDL::getRenderTarget(baseViewInfo.Monitor);
	SDL_SetRenderTarget(renderer_, vuMeterTexture_);
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
	SDL_RenderClear(renderer_);

	float barWidth = static_cast<float>(vuMeterTextureWidth_) / static_cast<float>(vuBarCount_);
	float barSpacing = barWidth * 0.1f;
	float actualBarWidth = barWidth - barSpacing;

	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

	for (int i = 0; i < vuBarCount_; i++) {
		float barX = static_cast<float>(i * barWidth);
		float barHeight = static_cast<float>(vuMeterTextureHeight_) * vuLevels_[i];
		float peakHeight = static_cast<float>(vuMeterTextureHeight_) * vuPeaks_[i];

		SDL_SetRenderDrawColor(renderer_, vuBackgroundColor_.r, vuBackgroundColor_.g, vuBackgroundColor_.b, 255);
		SDL_FRect bgRect = { barX, 0, actualBarWidth, static_cast<float>(vuMeterTextureHeight_) };
		SDL_RenderFillRectF(renderer_, &bgRect);

		float greenZone = vuMeterTextureHeight_ * vuGreenThreshold_;
		float yellowZone = vuMeterTextureHeight_ * (vuYellowThreshold_ - vuGreenThreshold_);
		float redZone = vuMeterTextureHeight_ * (1.0f - vuYellowThreshold_);

		if (barHeight > 0) {
			SDL_SetRenderDrawColor(renderer_, vuBottomColor_.r, vuBottomColor_.g, vuBottomColor_.b, 255);
			float segmentHeight = std::min(barHeight, greenZone);
			SDL_FRect greenRect = { barX + barSpacing * 0.5f, vuMeterTextureHeight_ - segmentHeight, actualBarWidth - barSpacing, segmentHeight };
			SDL_RenderFillRectF(renderer_, &greenRect);
		}
		if (barHeight > greenZone) {
			SDL_SetRenderDrawColor(renderer_, vuMiddleColor_.r, vuMiddleColor_.g, vuMiddleColor_.b, 255);
			float segmentHeight = std::min(barHeight - greenZone, yellowZone);
			SDL_FRect yellowRect = { barX + barSpacing * 0.5f, vuMeterTextureHeight_ - greenZone - segmentHeight, actualBarWidth - barSpacing, segmentHeight };
			SDL_RenderFillRectF(renderer_, &yellowRect);
		}
		if (barHeight > greenZone + yellowZone) {
			SDL_SetRenderDrawColor(renderer_, vuTopColor_.r, vuTopColor_.g, vuTopColor_.b, 255);
			float segmentHeight = std::min(barHeight - greenZone - yellowZone, redZone);
			SDL_FRect redRect = { barX + barSpacing * 0.5f, vuMeterTextureHeight_ - greenZone - yellowZone - segmentHeight, actualBarWidth - barSpacing, segmentHeight };
			SDL_RenderFillRectF(renderer_, &redRect);
		}
		if (peakHeight > 0 && peakHeight >= barHeight) {
			SDL_SetRenderDrawColor(renderer_, vuPeakColor_.r, vuPeakColor_.g, vuPeakColor_.b, 255);
			SDL_FRect peakRect = { barX + barSpacing * 0.5f, vuMeterTextureHeight_ - peakHeight - 2, actualBarWidth - barSpacing, 2 };
			SDL_RenderFillRectF(renderer_, &peakRect);
		}
	}
	SDL_SetRenderTarget(renderer_, previousTarget);
	vuMeterNeedsUpdate_ = false;
}


bool MusicPlayerComponent::parseHexColor(const std::string& hexString, SDL_Color& outColor) {
	std::string_view sv = hexString;
	if (sv.length() != 6) {
		LOG_WARNING("MusicPlayerComponent", "Invalid length for hex string: " + hexString);
		return false;
	}
	for (char c : sv) {
		if (!std::isxdigit(static_cast<unsigned char>(c))) {
			LOG_WARNING("parseHexColor", "Non-hex character found in string: " + hexString);
			return false;
		}
	}
	unsigned int r, g, b;
	auto result_r = std::from_chars(sv.data(), sv.data() + 2, r, 16);
	auto result_g = std::from_chars(sv.data() + 2, sv.data() + 4, g, 16);
	auto result_b = std::from_chars(sv.data() + 4, sv.data() + 6, b, 16);

	if (result_r.ec != std::errc() || result_g.ec != std::errc() || result_b.ec != std::errc() ||
		result_r.ptr != sv.data() + 2 ||
		result_g.ptr != sv.data() + 4 ||
		result_b.ptr != sv.data() + 6)
	{
		LOG_WARNING("MusicPlayerComponent", "Hex conversion failed for string: " + hexString);
		return false;
	}

	outColor.r = static_cast<Uint8>(r);
	outColor.g = static_cast<Uint8>(g);
	outColor.b = static_cast<Uint8>(b);
	outColor.a = 255;
	return true;
}

void MusicPlayerComponent::updateVuLevels() {
	const std::vector<float>& audioLevels = musicPlayer_->getAudioLevels();
	int channels = musicPlayer_->getAudioChannels();

	if (!musicPlayer_->isPlaying() || audioLevels.empty()) {
		for (auto& level : vuLevels_) { level *= 0.8f; }
		for (auto& peak : vuPeaks_) { peak *= 0.9f; }
		return;
	}

	const float amplification = 5.0f;

	if (vuMeterIsMono_) {
		float averageLevel = 0.0f;
		float sum = 0.0f;
		for (float level : audioLevels) { sum += level; }
		if (!audioLevels.empty()) { averageLevel = sum / static_cast<float>(audioLevels.size()); }
		float monoLevel = std::min(1.0f, averageLevel * amplification);
		for (int i = 0; i < vuBarCount_; i++) {
			float barPos = static_cast<float>(i) / vuBarCount_;
			float patternFactor;
			if (i % 2 == 0) { patternFactor = 1.0f - std::abs(barPos - 0.5f) * 0.6f; }
			else { patternFactor = 1.0f + 0.3f * std::sin(barPos * 3.14159f * 4); }
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
		if (channels >= 2) {
			int leftBars = vuBarCount_ / 2;
			int rightBars = vuBarCount_ - leftBars;
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
			if ((rand() % 10) < 3) {
				int barToBoost = rand() % vuBarCount_;
				vuLevels_[barToBoost] = std::min(1.0f, vuLevels_[barToBoost] * 1.3f);
				vuPeaks_[barToBoost] = std::max(vuPeaks_[barToBoost], vuLevels_[barToBoost]);
			}
		}
		else {
			float level = std::min(1.0f, audioLevels[0] * amplification);
			for (int i = 0; i < vuBarCount_; i++) {
				float randomFactor = 1.0f + ((rand() % 10) - 5) / 100.0f;
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
	if (!renderer_ || !vuMeterTexture_ || !musicPlayer_->hasStartedPlaying()) {
		return;
	}
	// createVuMeterTextureIfNeeded is called in draw() before this
	// updateVuMeterTexture is called in draw() if vuMeterNeedsUpdate_ is true
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

void MusicPlayerComponent::createProgressBarTextureIfNeeded() {
	if (!renderer_) return;

	int targetWidth = static_cast<int>(baseViewInfo.ScaledWidth());
	int targetHeight = static_cast<int>(baseViewInfo.ScaledHeight());

	if (targetWidth <= 0 || targetHeight <= 0) {
		if (progressBarTexture_ != nullptr) {
			SDL_DestroyTexture(progressBarTexture_);
			progressBarTexture_ = nullptr;
			progressBarTextureWidth_ = 0;
			progressBarTextureHeight_ = 0;
		}
		return;
	}

	if (progressBarTexture_ == nullptr ||
		progressBarTextureWidth_ != targetWidth ||
		progressBarTextureHeight_ != targetHeight) {

		if (progressBarTexture_ != nullptr) {
			SDL_DestroyTexture(progressBarTexture_);
			progressBarTexture_ = nullptr;
		}

		progressBarTextureWidth_ = targetWidth;
		progressBarTextureHeight_ = targetHeight;

		progressBarTexture_ = SDL_CreateTexture(
			renderer_,
			SDL_PIXELFORMAT_RGBA8888,
			SDL_TEXTUREACCESS_TARGET,
			progressBarTextureWidth_,
			progressBarTextureHeight_
		);

		if (progressBarTexture_) {
			SDL_SetTextureBlendMode(progressBarTexture_, SDL_BLENDMODE_BLEND);
			progressBarNeedsUpdate_ = true; // Force redraw after creation/resize
			LOG_INFO("MusicPlayerComponent", "Created/Resized progress bar texture: " +
				std::to_string(progressBarTextureWidth_) + "x" +
				std::to_string(progressBarTextureHeight_));
		}
		else {
			LOG_ERROR("MusicPlayerComponent", "Failed to create progress bar texture");
			progressBarTextureWidth_ = 0;
			progressBarTextureHeight_ = 0;
		}
	}
}

void MusicPlayerComponent::updateProgressBarTexture() {
	if (!renderer_ || !progressBarTexture_) {
		return;
	}

	SDL_Texture* previousTarget = SDL::getRenderTarget(baseViewInfo.Monitor);
	SDL_SetRenderTarget(renderer_, progressBarTexture_);

	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
	SDL_RenderClear(renderer_);

	float current = static_cast<float>(musicPlayer_->getCurrent());
	float duration = static_cast<float>(musicPlayer_->getDuration());
	float progressPercent = 0.0f;

	if (duration > 0.0f) {
		progressPercent = std::clamp(current / duration, 0.0f, 1.0f);
	}

	// Use texture dimensions
	float barWidth = static_cast<float>(progressBarTextureWidth_);
	float barHeight = static_cast<float>(progressBarTextureHeight_);

	if (barWidth <= 0 || barHeight <= 0) { // Should not happen if createProgressBarTextureIfNeeded works
		SDL_SetRenderTarget(renderer_, previousTarget);
		progressBarNeedsUpdate_ = false;
		lastProgressPercent_ = progressPercent;
		return;
	}

	float filledWidth = barWidth * progressPercent;
	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE); // Use opaque drawing for texture content

	// Background bar color (opaque black)
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
	SDL_FRect backgroundRect = { 0.0f, 0.0f, barWidth, barHeight };
	SDL_RenderFillRectF(renderer_, &backgroundRect);

	// Progress bar color (opaque white)
	SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
	if (filledWidth > 0) {
		SDL_FRect progressRect = { 0.0f, 0.0f, filledWidth, barHeight };
		SDL_RenderFillRectF(renderer_, &progressRect);
	}

	SDL_SetRenderTarget(renderer_, previousTarget);
	progressBarNeedsUpdate_ = false;
	lastProgressPercent_ = progressPercent; // Store the progress for which this texture was rendered
}

void MusicPlayerComponent::drawProgressBarTexture() {
	if (!renderer_ || !progressBarTexture_) {
		return;
	}

	SDL_FRect rect;
	rect.x = baseViewInfo.XRelativeToOrigin();
	rect.y = baseViewInfo.YRelativeToOrigin();
	rect.w = baseViewInfo.ScaledWidth();
	rect.h = baseViewInfo.ScaledHeight();

	SDL::renderCopyF(
		progressBarTexture_,
		baseViewInfo.Alpha, // Apply layout alpha when rendering the texture
		nullptr,
		&rect,
		baseViewInfo,
		page.getLayoutWidth(baseViewInfo.Monitor),
		page.getLayoutHeight(baseViewInfo.Monitor)
	);
}


void MusicPlayerComponent::drawAlbumArt() {
	if (!renderer_) {
		return;
	}
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
	std::vector<unsigned char> albumArtData;
	if (musicPlayer_->getAlbumArt(albumArtTrackIndex_, albumArtData) && !albumArtData.empty()) {
		SDL_RWops* rw = SDL_RWFromConstMem(albumArtData.data(), static_cast<int>(albumArtData.size()));
		if (rw) {
			albumArtTexture_ = IMG_LoadTexture_RW(renderer_, rw, 1);
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
	albumArtTexture_ = loadDefaultAlbumArt();
}

SDL_Texture* MusicPlayerComponent::loadDefaultAlbumArt() {
	std::string layoutName;
	config_.getProperty(OPTION_LAYOUT, layoutName);
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
	SDL_FRect rect;
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
	// ... (initial checks for album art, volume bar, etc. remain the same) ...
	if (isAlbumArt_ || isVolumeBar_ || isProgressBar_ || isVuMeter_ || !musicPlayer_->hasStartedPlaying()) {
		return nullptr;
	}

	std::string typeLC = Utils::toLower(type_);

	// --- Text-based components ---
	if (typeLC == "filename" || typeLC == "trackinfo" || typeLC == "title" ||
		typeLC == "artist" || typeLC == "album" || typeLC == "time" || typeLC == "volume") {

		std::string newTextValue;
		if (typeLC == "filename") {
			newTextValue = musicPlayer_->getCurrentTrackNameWithoutExtension();
		}
		else if (typeLC == "trackinfo") {
			newTextValue = musicPlayer_->getFormattedTrackInfo();
			if (newTextValue.empty()) newTextValue = "No track playing";
		}
		else if (typeLC == "title") {
			newTextValue = musicPlayer_->getCurrentTitle();
			if (newTextValue.empty()) newTextValue = "Unknown";
		}
		else if (typeLC == "artist") {
			newTextValue = musicPlayer_->getCurrentArtist();
			if (newTextValue.empty()) newTextValue = "Unknown Artist";
		}
		else if (typeLC == "album") {
			newTextValue = musicPlayer_->getCurrentAlbum();
			if (newTextValue.empty()) newTextValue = "Unknown Album";
		}
		else if (typeLC == "time") {
			auto [currentSec, durationSec] = musicPlayer_->getCurrentAndDurationSec();
			if (currentSec < 0) return loadedComponent_; // Or nullptr if no previous component

			int currentMin = currentSec / 60;
			int currentRemSec = currentSec % 60;
			int durationMin = durationSec / 60;
			int durationRemSec = durationSec % 60;

			std::stringstream ss;
			int minWidth = durationMin >= 10 ? 2 : 1;
			// Ensure duration is positive before calculating minWidth based on it.
			// If durationSec is 0 or less, default minWidth to 1 for current time.
			if (durationSec <= 0) minWidth = (currentMin >= 10) ? 2 : 1;


			ss << std::setfill('0') << std::setw(minWidth) << currentMin << ":"
				<< std::setfill('0') << std::setw(2) << currentRemSec;
			if (durationSec > 0) { // Only show duration if available
				ss << "/"
					<< std::setfill('0') << std::setw(minWidth) << durationMin << ":"
					<< std::setfill('0') << std::setw(2) << durationRemSec;
			}
			newTextValue = ss.str();
		}
		else if (typeLC == "volume") {
			int volumeRaw = musicPlayer_->getVolume();
			int volumePercentage = static_cast<int>((volumeRaw / 128.0f) * 100.0f + 0.5f);
			newTextValue = std::to_string(volumePercentage) + "%"; // Optional: add '%'
		}

		Text* textComponent = dynamic_cast<Text*>(loadedComponent_);
		if (textComponent) {
			// Check if text actually changed to avoid unnecessary texture re-creation
			if (textComponent->getText() != newTextValue) {
				textComponent->setText(newTextValue);
			}
		}
		else {
			// loadedComponent_ is not a Text component or is nullptr, create a new one
			if (loadedComponent_) { // It was something else, delete old one
				loadedComponent_->freeGraphicsMemory();
				delete loadedComponent_;
			}
			loadedComponent_ = new Text(newTextValue, page, font_, baseViewInfo.Monitor);
			// Since it's new, it needs allocation if it wasn't done by constructor
			// Assuming Text constructor or subsequent call handles allocateGraphicsMemory
			// If not: loadedComponent_->allocateGraphicsMemory(); (if Text needs it explicitly after creation)
		}
		return loadedComponent_;
	}

	// --- Image-based components (state, shuffle, loop) ---
	// This part can remain largely the same as creating new Image components
	// is generally less expensive than new Text components, and image paths might change.
	std::string basename;
	if (typeLC == "state") {
		// ... (state logic as before) ...
		MusicPlayer::PlaybackState state = musicPlayer_->getPlaybackState();
		// ... (logic to set basename: "next", "previous", "playing", "paused")
		if (state == MusicPlayer::PlaybackState::NEXT) basename = "next";
		else if (state == MusicPlayer::PlaybackState::PREVIOUS) basename = "previous";
		else if (state == MusicPlayer::PlaybackState::PLAYING) basename = "playing";
		else if (state == MusicPlayer::PlaybackState::PAUSED) basename = "paused";
		else basename = "unknown"; // Fallback
	}
	else if (typeLC == "shuffle") {
		basename = musicPlayer_->getShuffle() ? "on" : "off";
	}
	else if (typeLC == "loop" || typeLC == "repeat") {
		basename = musicPlayer_->getLoop() ? "on" : "off";
	}
	else {
		// Should not be reached if all types are handled above
		LOG_WARNING("MusicPlayerComponent", "Unhandled component type in reloadComponent: " + typeLC);
		return loadedComponent_; // Or nullptr
	}

	// Logic for image components (relatively unchanged but ensure proper cleanup)
	std::string layoutName;
	config_.getProperty(OPTION_LAYOUT, layoutName);
	std::string imagePathPrefix; // Determine this based on commonMode_ etc.
	if (commonMode_) {
		imagePathPrefix = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", "_common", "medium_artwork", typeLC);
	}
	else {
		imagePathPrefix = Utils::combinePath(Configuration::absolutePath, "music", typeLC); // Example, adjust as needed
	}

	ImageBuilder imageBuild{};
	Component* newImageComponent = imageBuild.CreateImage(imagePathPrefix, page, basename, baseViewInfo.Monitor, baseViewInfo.Additive, true);

	if (newImageComponent) {
		if (loadedComponent_ && loadedComponent_ != newImageComponent) { // If different or old one exists
			// Check if the file path actually changed to avoid needless reload
			// This requires Image component to expose its file path.
			Image* oldImage = dynamic_cast<Image*>(loadedComponent_);
			Image* newPotentialImage = dynamic_cast<Image*>(newImageComponent);
			bool pathChanged = true;
			if (oldImage && newPotentialImage && oldImage->filePath() == newPotentialImage->filePath()) {
				pathChanged = false;
			}

			if (pathChanged) {
				if (loadedComponent_) {
					loadedComponent_->freeGraphicsMemory();
					delete loadedComponent_;
				}
				loadedComponent_ = newImageComponent;
				loadedComponent_->allocateGraphicsMemory(); // Essential for new images
			}
			else {
				// Paths are the same, no need to replace. Delete the newly created one.
				newImageComponent->freeGraphicsMemory(); // Or just delete if it cleans up
				delete newImageComponent;
			}
		}
		else if (!loadedComponent_) { // No previous component
			loadedComponent_ = newImageComponent;
			loadedComponent_->allocateGraphicsMemory();
		}
		// If loadedComponent_ == newImageComponent (can happen if CreateImage returns existing), do nothing
	}
	else {
		// Failed to create image, potentially clear old one or log error
		if (loadedComponent_) {
			loadedComponent_->freeGraphicsMemory();
			delete loadedComponent_;
			loadedComponent_ = nullptr;
		}
		LOG_WARNING("MusicPlayerComponent", "Failed to create image for: " + typeLC + "/" + basename);
	}
	return loadedComponent_;
}
void MusicPlayerComponent::pause() {
	if (musicPlayer_->isPlaying()) musicPlayer_->pauseMusic();
	else musicPlayer_->playMusic();
}

unsigned long long MusicPlayerComponent::getCurrent() { return static_cast<unsigned long long>(musicPlayer_->getCurrent()); }
unsigned long long MusicPlayerComponent::getDuration() { return static_cast<unsigned long long>(musicPlayer_->getDuration()); }
bool MusicPlayerComponent::isPaused() { return musicPlayer_->isPaused(); }
bool MusicPlayerComponent::isPlaying() { return musicPlayer_->isPlaying(); }