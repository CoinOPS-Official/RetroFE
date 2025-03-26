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
	, refreshInterval_(0.25f)
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
	, isVuMeter_(Utils::toLower(type) == "vumeter")
	, vuBarCount_(40)  // Default number of bars
	, vuDecayRate_(2.0f)  // How quickly the bars fall
	, vuPeakDecayRate_(0.4f)  // How quickly the peak markers fall
	, vuGreenColor_({ 0, 220, 0, 255 })
	, vuYellowColor_({ 220, 220, 0, 255 })
	, vuRedColor_({ 220, 0, 0, 255 })
	, vuBackgroundColor_({ 40, 40, 40, 255 })
	, vuPeakColor_({ 255, 255, 255, 255 })
	, vuGreenThreshold_(0.4f)
	, vuYellowThreshold_(0.6f) {
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

	if (loadedComponent_ != nullptr) {
		loadedComponent_->freeGraphicsMemory();
		delete loadedComponent_;
		loadedComponent_ = nullptr;
	}
	cachedTextComponent_ = nullptr;
}

void MusicPlayerComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();

	// Get the renderer if we're going to handle album art or volume bar
	if (isAlbumArt_ || isVolumeBar_ || type_ == "progress" || isVuMeter_) {
		renderer_ = SDL::getRenderer(baseViewInfo.Monitor);
	}

	if (isVuMeter_) {
		musicPlayer_->registerVisualizerCallback();
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
		int r, g, b;
		if (config_.getProperty("musicPlayer.vuMeter.greenColor.r", r) &&
			config_.getProperty("musicPlayer.vuMeter.greenColor.g", g) &&
			config_.getProperty("musicPlayer.vuMeter.greenColor.b", b)) {
			vuGreenColor_.r = static_cast<Uint8>(std::max(0, std::min(255, r)));
			vuGreenColor_.g = static_cast<Uint8>(std::max(0, std::min(255, g)));
			vuGreenColor_.b = static_cast<Uint8>(std::max(0, std::min(255, b)));
		}

		if (config_.getProperty("musicPlayer.vuMeter.yellowColor.r", r) &&
			config_.getProperty("musicPlayer.vuMeter.yellowColor.g", g) &&
			config_.getProperty("musicPlayer.vuMeter.yellowColor.b", b)) {
			vuYellowColor_.r = static_cast<Uint8>(std::max(0, std::min(255, r)));
			vuYellowColor_.g = static_cast<Uint8>(std::max(0, std::min(255, g)));
			vuYellowColor_.b = static_cast<Uint8>(std::max(0, std::min(255, b)));
		}

		if (config_.getProperty("musicPlayer.vuMeter.redColor.r", r) &&
			config_.getProperty("musicPlayer.vuMeter.redColor.g", g) &&
			config_.getProperty("musicPlayer.vuMeter.redColor.b", b)) {
			vuRedColor_.r = static_cast<Uint8>(std::max(0, std::min(255, r)));
			vuRedColor_.g = static_cast<Uint8>(std::max(0, std::min(255, g)));
			vuRedColor_.b = static_cast<Uint8>(std::max(0, std::min(255, b)));
		}

		if (config_.getProperty("musicPlayer.vuMeter.backgroundColor.r", r) &&
			config_.getProperty("musicPlayer.vuMeter.backgroundColor.g", g) &&
			config_.getProperty("musicPlayer.vuMeter.backgroundColor.b", b)) {
			vuBackgroundColor_.r = static_cast<Uint8>(std::max(0, std::min(255, r)));
			vuBackgroundColor_.g = static_cast<Uint8>(std::max(0, std::min(255, g)));
			vuBackgroundColor_.b = static_cast<Uint8>(std::max(0, std::min(255, b)));
		}

		if (config_.getProperty("musicPlayer.vuMeter.peakColor.r", r) &&
			config_.getProperty("musicPlayer.vuMeter.peakColor.g", g) &&
			config_.getProperty("musicPlayer.vuMeter.peakColor.b", b)) {
			vuPeakColor_.r = static_cast<Uint8>(std::max(0, std::min(255, r)));
			vuPeakColor_.g = static_cast<Uint8>(std::max(0, std::min(255, g)));
			vuPeakColor_.b = static_cast<Uint8>(std::max(0, std::min(255, b)));
		}

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

void MusicPlayerComponent::updateVolumeBarTexture() {
	if (!renderer_ || !volumeEmptyTexture_ || !volumeFullTexture_ || !volumeBarTexture_) {
		return;
	}

	// Get raw volume (0–128)
	int volumeRaw = musicPlayer_->getLogicalVolume();
	volumeRaw = std::clamp(volumeRaw, 0, 128); // Just in case

	// Compute visible width proportionally (integer math)
	int visibleWidth = (volumeBarWidth_ * volumeRaw) / 128;

	// Set render target to volume bar texture
	SDL_SetRenderTarget(renderer_, volumeBarTexture_);

	// Clear with full transparency
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
	SDL_RenderClear(renderer_);

	// Draw full portion only if volume > 0
	if (visibleWidth > 0) {
		SDL_Rect srcRect = { 0, 0, visibleWidth, volumeBarHeight_ };
		SDL_Rect dstRect = { 0, 0, visibleWidth, volumeBarHeight_ };
		SDL_RenderCopy(renderer_, volumeFullTexture_, &srcRect, &dstRect);
	}

	// Draw empty portion only if volume < 128
	if (visibleWidth < volumeBarWidth_) {
		SDL_Rect srcRect = { visibleWidth, 0, volumeBarWidth_ - visibleWidth, volumeBarHeight_ };
		SDL_Rect dstRect = { visibleWidth, 0, volumeBarWidth_ - visibleWidth, volumeBarHeight_ };
		SDL_RenderCopy(renderer_, volumeEmptyTexture_, &srcRect, &dstRect);
	}

	// Reset render target
	SDL_SetRenderTarget(renderer_, nullptr);

	// Save last volume value
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

		return Component::update(dt);
	}


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
		if (musicPlayer_->getButtonPressed()) {
			// Volume changed by user input — refresh bar
			updateVolumeBarTexture();
			volumeChanging_ = true;
			volumeStableTimer_ = 0.0f;
			musicPlayer_->setButtonPressed(false);
		}
		else {
			// No user input — increment stable timer
			volumeStableTimer_ += dt;

			// After delay, stop considering it "changing"
			if (volumeChanging_ && volumeStableTimer_ >= volumeFadeDelay_) {
				volumeChanging_ = false;
			}
		}

		// Respect layout alpha override — this takes precedence
		if (baseViewInfo.Alpha <= 0.0f) {
			// Layout says: fully invisible — suppress our own fade logic
			targetAlpha_ = 0.0f;
			currentDisplayAlpha_ = 0.0f;  // Ensure alpha is clamped
			volumeStableTimer_ = 0.0f;    // Reset timer
			volumeChanging_ = false;     // Cancel any ongoing fade
		}
		else {
			// Layout allows visibility — resume our logic
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
		currentState = std::to_string(musicPlayer_->getCurrent());
	}
	else if (type_ == "progress") {
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

	if (isVuMeter_) {
		if (baseViewInfo.Alpha > 0.0f) {
			drawVuMeter();
		}
		return;
	}

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

	if (type_ == "progress") {
		drawProgressBar();
		return;
	}

	// Draw the text component if it exists...
	if (cachedTextComponent_) {
		cachedTextComponent_->baseViewInfo = baseViewInfo;
		if (baseViewInfo.Alpha > 0.0f) {
			cachedTextComponent_->draw();
		}
	}
	// Otherwise, fall back to the loaded component if available.
	else if (loadedComponent_ != nullptr) {
		loadedComponent_->baseViewInfo = baseViewInfo;
		if (baseViewInfo.Alpha > 0.0f) {
			loadedComponent_->draw();
		}
	}
}

void MusicPlayerComponent::updateVuLevels() {
	// Get audio levels from the music player
	const std::vector<float>& audioLevels = musicPlayer_->getAudioLevels();
	int channels = musicPlayer_->getAudioChannels();

	if (!musicPlayer_->isPlaying() || audioLevels.empty()) {
		// If not playing, rapidly reduce all levels for quick response
		for (auto& level : vuLevels_) {
			level *= 0.8f; // Faster decay when not playing
		}
		for (auto& peak : vuPeaks_) {
			peak *= 0.9f;
		}
		return;
	}

	// Amplification factor - make the visualization more dramatic
	// This can be exposed to configuration if desired
	const float amplification = 5.0f; // Significant boost to the levels

	// Distribute audio levels across VU bars
	if (channels >= 2) {
		// Stereo mode: left channel for left half, right channel for right half
		int leftBars = vuBarCount_ / 2;
		int rightBars = vuBarCount_ - leftBars;

		// Left channel (first half of bars)
		float leftLevel = std::min(1.0f, audioLevels[0] * amplification); // Amplify and clamp
		for (int i = 0; i < leftBars; i++) {
			// Use exponential distribution to create more dramatic effect
			// Lower frequencies (lower bar indexes) get higher values
			float barFactor = 1.0f - (static_cast<float>(i) / leftBars) * 0.5f; // Less falloff

			// Add dynamic randomness that scales with level
			float randomFactor = 1.0f + ((rand() % 20) - 10) / 100.0f; // ±10% variation

			// Calculate new level with enhanced dynamics
			float newLevel = leftLevel * barFactor * randomFactor;

			// Apply non-linear scaling to emphasize higher levels
			newLevel = std::min(1.0f, std::pow(newLevel, 0.8f)); // Power < 1 emphasizes higher values

			// Apply if higher than current
			if (newLevel > vuLevels_[i]) {
				vuLevels_[i] = newLevel;

				// Update peak
				vuPeaks_[i] = std::max(vuPeaks_[i], newLevel);
			}
		}

		// Right channel (second half of bars)
		float rightLevel = std::min(1.0f, audioLevels[1] * amplification); // Amplify and clamp
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

		// Add extra dynamics: occasionally boost random bars for a more lively display
		// This simulates frequency spikes that occur in music
		if ((rand() % 10) < 3) { // 30% chance each update
			int barToBoost = rand() % vuBarCount_;
			vuLevels_[barToBoost] = std::min(1.0f, vuLevels_[barToBoost] * 1.3f);
			vuPeaks_[barToBoost] = std::max(vuPeaks_[barToBoost], vuLevels_[barToBoost]);
		}
	}
	else {
		// Mono mode: create a more interesting pattern
		float monoLevel = std::min(1.0f, audioLevels[0] * amplification); // Amplify and clamp

		for (int i = 0; i < vuBarCount_; i++) {
			// Create a pattern that emphasizes both center and edges
			float barPos = static_cast<float>(i) / vuBarCount_;
			float patternFactor;

			// Use a combination of patterns for more interesting visualization
			if (i % 2 == 0) {
				// For even bars, emphasize center
				patternFactor = 1.0f - std::abs(barPos - 0.5f) * 0.6f;
			}
			else {
				// For odd bars, create a wave pattern
				patternFactor = 0.7f + 0.3f * std::sin(barPos * 3.14159f * 4);
			}

			// Add dynamic randomness
			float randomFactor = 1.0f + ((rand() % 25) - 10) / 100.0f; // -10% to +15% variation

			float newLevel = monoLevel * patternFactor * randomFactor;
			newLevel = std::min(1.0f, std::pow(newLevel, 0.75f)); // More aggressive curve

			if (newLevel > vuLevels_[i]) {
				vuLevels_[i] = newLevel;
				vuPeaks_[i] = std::max(vuPeaks_[i], newLevel);
			}
		}

		// Occasionally create "wave" effects across the bars
		static int wavePosition = 0;
		static bool waveActive = false;

		if (!waveActive && (rand() % 20) < 3) { // 15% chance to start a wave
			waveActive = true;
			wavePosition = 0;
		}

		if (waveActive) {
			// Create a moving wave effect
			float waveAmplitude = 0.3f * monoLevel; // Scale with audio level
			int waveWidth = vuBarCount_ / 3;

			for (int i = 0; i < vuBarCount_; i++) {
				// Calculate distance from wave center
				int distance = std::abs(i - wavePosition);
				if (distance < waveWidth) {
					// Apply wave effect with falloff from center
					float waveFactor = waveAmplitude * (1.0f - static_cast<float>(distance) / waveWidth);
					vuLevels_[i] = std::min(1.0f, vuLevels_[i] + waveFactor);
					vuPeaks_[i] = std::max(vuPeaks_[i], vuLevels_[i]);
				}
			}

			// Move the wave
			wavePosition += 1;
			if (wavePosition >= vuBarCount_ + waveWidth) {
				waveActive = false;
			}
		}
	}
}

void MusicPlayerComponent::drawVuMeter() {
	if (!renderer_ || baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	// Calculate the dimensions and position of the VU meter
	float meterX = baseViewInfo.XRelativeToOrigin();
	float meterY = baseViewInfo.YRelativeToOrigin();
	float meterWidth = baseViewInfo.ScaledWidth();
	float meterHeight = baseViewInfo.ScaledHeight();

	// Calculate bar dimensions
	float barWidth = meterWidth / vuBarCount_;
	float barSpacing = barWidth * 0.1f; // 10% of bar width for spacing
	float actualBarWidth = barWidth - barSpacing;

	// Set blend mode for transparency
	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

	// Draw each bar
	for (int i = 0; i < vuBarCount_; i++) {
		float barX = meterX + i * barWidth;

		// Calculate the height of this bar based on its level
		float barHeight = meterHeight * vuLevels_[i];
		float peakHeight = meterHeight * vuPeaks_[i];

		// Background/border for bar
		SDL_SetRenderDrawColor(
			renderer_,
			vuBackgroundColor_.r,
			vuBackgroundColor_.g,
			vuBackgroundColor_.b,
			static_cast<Uint8>(baseViewInfo.Alpha * 255)
		);
		SDL_FRect bgRect = { barX, meterY, actualBarWidth, meterHeight };
		SDL_RenderFillRectF(renderer_, &bgRect);

		// Calculate zone heights based on thresholds
		float greenZone = meterHeight * vuGreenThreshold_;
		float yellowZone = meterHeight * (vuYellowThreshold_ - vuGreenThreshold_);
		float redZone = meterHeight * (1.0f - vuYellowThreshold_);

		// Draw the green segment
		if (barHeight > 0) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuGreenColor_.r,
				vuGreenColor_.g,
				vuGreenColor_.b,
				static_cast<Uint8>(baseViewInfo.Alpha * 255)
			);
			float segmentHeight = std::min(barHeight, greenZone);
			SDL_FRect greenRect = {
				barX + barSpacing * 0.5f,
				meterY + meterHeight - segmentHeight,
				actualBarWidth - barSpacing,
				segmentHeight
			};
			SDL_RenderFillRectF(renderer_, &greenRect);
		}

		// Draw the yellow segment
		if (barHeight > greenZone) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuYellowColor_.r,
				vuYellowColor_.g,
				vuYellowColor_.b,
				static_cast<Uint8>(baseViewInfo.Alpha * 255)
			);
			float segmentHeight = std::min(barHeight - greenZone, yellowZone);
			SDL_FRect yellowRect = {
				barX + barSpacing * 0.5f,
				meterY + meterHeight - greenZone - segmentHeight,
				actualBarWidth - barSpacing,
				segmentHeight
			};
			SDL_RenderFillRectF(renderer_, &yellowRect);
		}

		// Draw the red segment
		if (barHeight > greenZone + yellowZone) {
			SDL_SetRenderDrawColor(
				renderer_,
				vuRedColor_.r,
				vuRedColor_.g,
				vuRedColor_.b,
				static_cast<Uint8>(baseViewInfo.Alpha * 255)
			);
			float segmentHeight = std::min(barHeight - greenZone - yellowZone, redZone);
			SDL_FRect redRect = {
				barX + barSpacing * 0.5f,
				meterY + meterHeight - greenZone - yellowZone - segmentHeight,
				actualBarWidth - barSpacing,
				segmentHeight
			};
			SDL_RenderFillRectF(renderer_, &redRect);
		}

		// Draw peak marker
		if (peakHeight > 0 && peakHeight >= barHeight) {
			// Use custom peak color
			SDL_SetRenderDrawColor(
				renderer_,
				vuPeakColor_.r,
				vuPeakColor_.g,
				vuPeakColor_.b,
				static_cast<Uint8>(baseViewInfo.Alpha * 255)
			);

			// Draw the peak marker as a thin line
			SDL_FRect peakRect = {
				barX + barSpacing * 0.5f,
				meterY + meterHeight - peakHeight - 2,
				actualBarWidth - barSpacing,
				2 // 2-pixel height for the peak marker
			};
			SDL_RenderFillRectF(renderer_, &peakRect);
		}
	}
}


void MusicPlayerComponent::drawProgressBar() {
	if (!renderer_ || baseViewInfo.Alpha <= 0.0f) {
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
				LOG_INFO("MusicPlayerComponent", "Loaded default album art from: " + path);
				return texture;
			}
		}
	}

	LOG_WARNING("MusicPlayerComponent", "Failed to load default album art");
	return nullptr;
}

void MusicPlayerComponent::drawVolumeBar() {
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

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(fileName);
		}
		else {
			cachedTextComponent_ = new Text(fileName, page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
		return loadedComponent_;
	}
	else if (typeLC == "trackinfo") {
		std::string trackName = musicPlayer_->getFormattedTrackInfo();
		if (trackName.empty()) {
			trackName = "No track playing";
		}

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(trackName);
		}
		else {
			cachedTextComponent_ = new Text(trackName, page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
		return loadedComponent_;
	}
	else if (typeLC == "title") {
		std::string titleName = musicPlayer_->getCurrentTitle();
		if (titleName.empty()) {
			titleName = "Unknown";
		}

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(titleName);
		}
		else {
			cachedTextComponent_ = new Text(titleName, page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
		return loadedComponent_;
	}
	else if (typeLC == "artist") {
		std::string artistName = musicPlayer_->getCurrentArtist();
		if (artistName.empty()) {
			artistName = "Unknown Artist";
		}

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(artistName);
		}
		else {
			cachedTextComponent_ = new Text(artistName, page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
		return loadedComponent_;
	}
	else if (typeLC == "album") {
		std::string albumName = musicPlayer_->getCurrentAlbum();
		if (albumName.empty()) {
			albumName = "Unknown Album";
		}

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(albumName);
		}
		else {
			cachedTextComponent_ = new Text(albumName, page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
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

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(ss.str());
		}
		else {
			cachedTextComponent_ = new Text(ss.str(), page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
		return loadedComponent_;
	}
	else if (typeLC == "progress") {

	}
	else if (typeLC == "volume") {
		int volumeRaw = musicPlayer_->getVolume();
		int volumePercentage = static_cast<int>((volumeRaw / 128.0f) * 100.0f + 0.5f);
		std::string volumeStr = std::to_string(volumePercentage);

		if (cachedTextComponent_) {
			cachedTextComponent_->setText(volumeStr);
		}
		else {
			cachedTextComponent_ = new Text(volumeStr, page, font_, baseViewInfo.Monitor);
		}
		// Now assign the text component as the single owner.
		loadedComponent_ = cachedTextComponent_;
		// Clear cachedTextComponent_ so only loadedComponent_ owns it.
		cachedTextComponent_ = nullptr;
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