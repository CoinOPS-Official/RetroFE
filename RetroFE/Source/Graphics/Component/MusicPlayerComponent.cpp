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
#include "kiss_fft.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <charconv>
#include <numeric>

static const SDL_BlendMode softOverlayBlendMode = SDL_ComposeCustomBlendMode(
	SDL_BLENDFACTOR_SRC_ALPHA,           // Source color factor: modulates source color by the alpha value set dynamically
	SDL_BLENDFACTOR_ONE,                 // Destination color factor: keep the destination as is
	SDL_BLENDOPERATION_ADD,              // Color operation: add source and destination colors based on alpha
	SDL_BLENDFACTOR_ONE,                 // Source alpha factor
	SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, // Destination alpha factor: inverse of source alpha
	SDL_BLENDOPERATION_ADD               // Alpha operation: add alpha values
);

bool parseHexColor(const std::string& hexString, SDL_Color& outColor) {
	std::string_view sv = hexString;
	if (sv.length() != 6) return false;
	for (char c : sv) {
		if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
	}
	unsigned int r, g, b;
	auto result_r = std::from_chars(sv.data(), sv.data() + 2, r, 16);
	auto result_g = std::from_chars(sv.data() + 2, sv.data() + 4, g, 16);
	auto result_b = std::from_chars(sv.data() + 4, sv.data() + 6, b, 16);
	if (result_r.ec != std::errc() || result_g.ec != std::errc() || result_b.ec != std::errc() ||
		result_r.ptr != sv.data() + 2 || result_g.ptr != sv.data() + 4 || result_b.ptr != sv.data() + 6) {
		return false;
	}
	outColor.r = static_cast<Uint8>(r);
	outColor.g = static_cast<Uint8>(g);
	outColor.b = static_cast<Uint8>(b);
	outColor.a = 255;
	return true;
}

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
	, gstreamerVisType_(GStreamerVisType::None)
	, totalSegments_{ 0 }
	, useSegmentedVolume_{ false }
	, isIsoVisualizer_(Utils::toLower(type) == "iso")
	, isVuMeter_(Utils::toLower(type) == "vumeter")
	, vuMeterNeedsUpdate_{ true } // State flag remains
{
	// Set the monitor for this component
	baseViewInfo.Monitor = monitor;

	// Get refresh interval from config if available
	int configRefreshInterval;
	if (config.getProperty("musicPlayer.refreshRate", configRefreshInterval)) {
		refreshInterval_ = static_cast<float>(configRefreshInterval) / 1000.0f; // Convert from ms to seconds
	}

	std::string typeLower = Utils::toLower(type);
	if (typeLower == "goom") gstreamerVisType_ = GStreamerVisType::Goom;
	else if (typeLower == "wavescope") gstreamerVisType_ = GStreamerVisType::Wavescope;
	else if (typeLower == "synaescope") gstreamerVisType_ = GStreamerVisType::Synaescope;
	else if (typeLower == "spectrascope") gstreamerVisType_ = GStreamerVisType::Spectrascope;
	else gstreamerVisType_ = GStreamerVisType::None;

	allocateGraphicsMemory();
}

namespace ImageProcessorConstants {
	const float SCAN_AREA_TOP_RATIO = 0.3f;
	const float SCAN_AREA_BOTTOM_RATIO = 0.9f;
	const Uint8 ALPHA_THRESHOLD = 50;
	const float LUMINANCE_JUMP_THRESHOLD = 50.0f;
	const int MIN_SEGMENT_WIDTH = 2;
}

MusicPlayerComponent::~MusicPlayerComponent(){
	freeGraphicsMemory();
	if (loadedComponent_ != nullptr) {
		//loadedComponent_->freeGraphicsMemory();
		delete loadedComponent_;
		loadedComponent_ = nullptr;
	}
}


void MusicPlayerComponent::freeGraphicsMemory() {
	Component::freeGraphicsMemory();

	if (isFftVisualizer() || gstreamerVisType_ != GStreamerVisType::None) {
		musicPlayer_->removeVisualizerListener(this);
	}

	if(fftTexture_ != nullptr) {
		SDL_DestroyTexture(fftTexture_);
		fftTexture_ = nullptr;
		fftTexW_ = fftTexH_ = 0;
	}

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

	if (progressBarTexture_ != nullptr) {
		SDL_DestroyTexture(progressBarTexture_);
		progressBarTexture_ = nullptr;
	}

	if (albumArtTexture_) {
		SDL_DestroyTexture(albumArtTexture_);
		albumArtTexture_ = nullptr;
	}

	if (gstPipeline_) {
		// Set pipeline state to NULL before unref (ensures clean shutdown)
		gst_element_set_state(gstPipeline_, GST_STATE_NULL);
		gst_object_unref(gstPipeline_);
		gstPipeline_ = nullptr;
	}

	if (gstTexture_ != nullptr) {
		SDL_DestroyTexture(gstTexture_);
		gstTexture_ = nullptr;
	}

	if (isFftVisualizer_ && kissfft_cfg_) {
		kiss_fftr_free(kissfft_cfg_); // Use the REAL FFT free function
		kissfft_cfg_ = nullptr;
	}

	if (loadedComponent_ != nullptr) {
		//loadedComponent_->freeGraphicsMemory();
		delete loadedComponent_;
		loadedComponent_ = nullptr;
	}
}

void MusicPlayerComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();

	// Get the renderer if we're going to handle album art or volume bar or progress bar or VU meter
	if (isAlbumArt_ || isVolumeBar_ || isProgressBar_ || gstreamerVisType_ != GStreamerVisType::None || isFftVisualizer()) {
		renderer_ = SDL::getRenderer(baseViewInfo.Monitor);
	}

	if (isFftVisualizer()) {
		if (!kissfft_cfg_) {
			// Use the REAL FFT allocation function
			kissfft_cfg_ = kiss_fftr_alloc(FFT_SIZE, 0, nullptr, nullptr);

			pcmBuffer_.clear();
			// Size the output buffer correctly.
			fftOutput_.assign(FFT_SIZE / 2 + 1, { 0.0f, 0.0f });
			fftMagnitudes_.assign(NR_OF_FREQ, 0.0f);
		}
		musicPlayer_->addVisualizerListener(this);

		// Shared texture for all FFT visualizers
		fftTexture_ = nullptr;
		fftTexW_ = fftTexH_ = 0;

		if (isIsoVisualizer_) {
			iso_grid_.assign(ISO_HISTORY, std::vector<IsoPoint>(NR_OF_FREQ));
			const int base_spacing_x = 8;
			const int base_spacing_y = 12;

			for (int i = 0; i < ISO_HISTORY; ++i) {
				for (int j = 0; j < NR_OF_FREQ; ++j) {
					iso_grid_[i][j].x = (j - (NR_OF_FREQ / 2)) * base_spacing_x;
					iso_grid_[i][j].y = i * base_spacing_y;
					iso_grid_[i][j].z = 0.0f;
				}
			}
		}

		if (isVuMeter_) {
			loadVuMeterConfig();
			vuLevels_.assign(vuMeterConfig_.barCount, 0.0f);
			vuPeaks_.assign(vuMeterConfig_.barCount, 0.0f);
		}
	}

	if (gstreamerVisType_ != GStreamerVisType::None) {
		musicPlayer_->addVisualizerListener(this);
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
	else if (!isAlbumArt_ && !isVolumeBar_ && !isProgressBar_ && gstreamerVisType_ == GStreamerVisType::None && !isFftVisualizer()) {
		loadedComponent_ = reloadComponent();
		if (loadedComponent_ != nullptr) {
			loadedComponent_->allocateGraphicsMemory();
		}
	}
}

void MusicPlayerComponent::onPcmDataReceived(const Uint8* data, int len) {
	// If this component is any type of visualizer, queue the data.
	if (isFftVisualizer() || gstreamerVisType_ != GStreamerVisType::None) {
		std::lock_guard<std::mutex> lock(pcmMutex_);
		pcmQueue_.emplace_back(data, data + len);
		while (pcmQueue_.size() > 10) {
			pcmQueue_.pop_front();
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

void MusicPlayerComponent::pushToGst(const Uint8* data, int len) {
	if (!gstAppSrc_) return;
	GstBuffer* buffer = gst_buffer_new_allocate(nullptr, len, nullptr);
	gst_buffer_fill(buffer, 0, data, len);

	// -- Add this block to set timestamps! --
	static GstClockTime pts = 0;
	// Each audio frame: bytes_per_sample * num_channels = 4 for S16LE stereo, 2 for S16LE mono, etc.
	const int bytes_per_frame = 2 /*bytes/sample*/ * 2 /*channels*/; // assuming S16LE, stereo
	const int sample_rate = 44100; // or whatever your pipeline uses

	// Number of frames (samples per channel) in this buffer:
	int nframes = len / bytes_per_frame;

	// Duration in nanoseconds for this buffer
	GstClockTime duration = (GstClockTime)((nframes * GST_SECOND) / sample_rate);

	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = duration;
	pts += duration;
	// ----------------------------------------

	gst_app_src_push_buffer(GST_APP_SRC(gstAppSrc_), buffer);
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

	if (isFftVisualizer()) {
		if (isIsoVisualizer_) {
			// updateIsoFFT now returns true if it updated fftMagnitudes_.
			if (updateIsoFFT()) {
				// We only need to flag for a redraw if there's new data.
				isoNeedsUpdate_ = true;
			}
			updateIsoState(dt);
		}
		else if (isVuMeter_) {
			updateVuMeterFFT(dt); 
			vuMeterNeedsUpdate_ = true;
		}
		int targetW = static_cast<int>(baseViewInfo.ScaledWidth());
		int targetH = static_cast<int>(baseViewInfo.ScaledHeight());
		if (!fftTexture_ || fftTexW_ != targetW || fftTexH_ != targetH) {
			if (fftTexture_) SDL_DestroyTexture(fftTexture_);

			// Ensure we don't try to create a 0x0 texture
			if (targetW > 0 && targetH > 0) {
				fftTexture_ = SDL_CreateTexture(
					renderer_,
					SDL_PIXELFORMAT_RGBA8888,
					SDL_TEXTUREACCESS_TARGET,
					targetW,
					targetH
				);
				if (fftTexture_) {
					SDL_SetTextureBlendMode(fftTexture_, softOverlayBlendMode);
					fftTexW_ = targetW;
					fftTexH_ = targetH;
					if (isVuMeter_) vuMeterNeedsUpdate_ = true; // Force redraw on new texture
				}
				else {
					LOG_ERROR("MusicPlayerComponent", "Failed to create FFT texture.");
					fftTexW_ = 0;
					fftTexH_ = 0;
				}
			}
			else {
				// Dimensions are invalid, so ensure texture is null
				fftTexture_ = nullptr;
				fftTexW_ = 0;
				fftTexH_ = 0;
			}
		}
		return Component::update(dt);
	}
	if (gstreamerVisType_ != GStreamerVisType::None) {
		// --- RESTORED LOGIC ---
		// This loop now runs on the main thread, pulling from our private queue.
		while (true) {
			std::vector<Uint8> pcmBlock;
			{
				std::lock_guard<std::mutex> lock(pcmMutex_);
				if (pcmQueue_.empty()) {
					break; // No more data this frame
				}
				pcmBlock = std::move(pcmQueue_.front());
				pcmQueue_.pop_front();
			}

			// This can now safely block without hanging the audio thread.
			pushToGst(pcmBlock.data(), static_cast<int>(pcmBlock.size()));
		}

		updateGstTextureFromAppSink();
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
				//loadedComponent_->freeGraphicsMemory();
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

void MusicPlayerComponent::updateIsoState(float dt) {
	if (fftMagnitudes_.empty()) return;

	// 1. Advance scroll progress based on a constant rate and elapsed time.
	// This is the key to smooth, frame-rate independent animation.
	iso_scroll_offset_ += iso_scroll_rate_ * dt;

	// 2. If we have scrolled past a full row, update the backing data grid.
	if (iso_scroll_offset_ >= 1.0f) {
		// Shift all Z values down one row.
		for (int i = ISO_HISTORY - 1; i > 0; --i) {
			for (int j = 0; j < NR_OF_FREQ; ++j)
				iso_grid_[i][j].z = iso_grid_[i - 1][j].z;
		}

		// Populate the top row with new FFT data.
		const float amplitude = 10.0f;
		const float logScale = 30.0f;
		for (int j = 1; j < NR_OF_FREQ; ++j) {
			float pre_emphasis = std::sqrt((float)j / (float)NR_OF_FREQ);
			float mag = fftMagnitudes_[j] * pre_emphasis;
			iso_grid_[0][j].z = amplitude * std::log2f(1 + logScale * mag);
		}
		iso_grid_[0][0].z = iso_grid_[0][1].z; // Mirror bin

		// Decrement offset, keeping the remainder for the next frame's calculation.
		iso_scroll_offset_ -= 1.0f;
	}

	// 3. Update beat pulse (optional animation flair).
	iso_beat_pulse_ *= (1.0f - (3.0f * dt)); // Frame-rate independent decay
}

// In MusicPlayerComponent.cpp

void MusicPlayerComponent::loadVuMeterConfig() {
	// Load boolean for mono/stereo mode
	config_.getProperty("musicPlayer.vuMeter.mono", vuMeterConfig_.isMono);

	// Load integer for the number of bars
	int configInt;
	if (config_.getProperty("musicPlayer.vuMeter.barCount", configInt)) {
		vuMeterConfig_.barCount = std::max(1, configInt);
	}

	// Load float values for decay rates and visual tuning
	float configFloat;
	if (config_.getProperty("musicPlayer.vuMeter.decayRate", configFloat)) {
		vuMeterConfig_.decayRate = std::max(0.1f, configFloat);
	}
	if (config_.getProperty("musicPlayer.vuMeter.peakDecayRate", configFloat)) {
		vuMeterConfig_.peakDecayRate = std::max(0.1f, configFloat);
	}
	if (config_.getProperty("musicPlayer.vuMeter.amplification", configFloat)) {
		vuMeterConfig_.amplification = std::max(0.1f, configFloat);
	}
	if (config_.getProperty("musicPlayer.vuMeter.curvePower", configFloat)) {
		vuMeterConfig_.curvePower = std::clamp(configFloat, 0.1f, 2.0f);
	}

	// Load float values for color thresholds
	if (config_.getProperty("musicPlayer.vuMeter.greenThreshold", configFloat)) {
		vuMeterConfig_.greenThreshold = std::clamp(configFloat, 0.0f, 1.0f);
	}
	if (config_.getProperty("musicPlayer.vuMeter.yellowThreshold", configFloat)) {
		// Ensure yellow threshold is always >= green threshold
		vuMeterConfig_.yellowThreshold = std::clamp(configFloat, vuMeterConfig_.greenThreshold, 1.0f);
	}

	// Load color strings and parse them
	std::string colorStr;
	SDL_Color parsedColor;
	if (config_.getProperty("musicPlayer.vuMeter.bottomColor", colorStr) && parseHexColor(colorStr, parsedColor)) {
		vuMeterConfig_.bottomColor = parsedColor;
	}
	if (config_.getProperty("musicPlayer.vuMeter.middleColor", colorStr) && parseHexColor(colorStr, parsedColor)) {
		vuMeterConfig_.middleColor = parsedColor;
	}
	if (config_.getProperty("musicPlayer.vuMeter.topColor", colorStr) && parseHexColor(colorStr, parsedColor)) {
		vuMeterConfig_.topColor = parsedColor;
	}
	if (config_.getProperty("musicPlayer.vuMeter.backgroundColor", colorStr) && parseHexColor(colorStr, parsedColor)) {
		vuMeterConfig_.backgroundColor = parsedColor;
	}
	if (config_.getProperty("musicPlayer.vuMeter.peakColor", colorStr) && parseHexColor(colorStr, parsedColor)) {
		vuMeterConfig_.peakColor = parsedColor;
	}
}

// Minimal replacement for MusicPlayerComponent::updateVuMeterFFT
void MusicPlayerComponent::updateVuMeterFFT(float dt) {
	if (!fillPcmBuffer() || !kissfft_cfg_) return;

	// --- FFT Processing ---
	std::vector<kiss_fft_scalar> fftInput(FFT_SIZE);
	std::copy(pcmBuffer_.begin(), pcmBuffer_.begin() + FFT_SIZE, fftInput.begin());

	// Erase from the main buffer immediately after copying (hop size = half window for smoothness)
	pcmBuffer_.erase(pcmBuffer_.begin(), pcmBuffer_.begin() + FFT_SIZE / 2);

	kiss_fftr(kissfft_cfg_, fftInput.data(), fftOutput_.data());

	// Compute magnitudes (NR_OF_FREQ = FFT_SIZE/2 + 1 for kiss_fftr)
	std::vector<float> magnitudes(NR_OF_FREQ);
	for (int i = 0; i < NR_OF_FREQ; ++i)
		magnitudes[i] = std::sqrt(fftOutput_[i].r * fftOutput_[i].r + fftOutput_[i].i * fftOutput_[i].i);

	int barCount = vuMeterConfig_.barCount;
	if (vuLevels_.size() != barCount)
		vuLevels_.assign(barCount, 0.0f);

	// Linear mapping: group bins into bars
	int binsPerBar = NR_OF_FREQ / barCount;
	for (int b = 0; b < barCount; ++b) {
		float sum = 0.0f;
		for (int i = 0; i < binsPerBar; ++i) {
			int idx = b * binsPerBar + i;
			if (idx < NR_OF_FREQ)
				sum += magnitudes[idx];
		}
		vuLevels_[b] = binsPerBar > 0 ? sum / binsPerBar : 0.0f;
	}

	// Normalize
	float maxVal = *std::max_element(vuLevels_.begin(), vuLevels_.end());
	if (maxVal > 0.0f)
		for (auto& v : vuLevels_) v /= maxVal;

	vuMeterNeedsUpdate_ = true;
}

bool MusicPlayerComponent::fillPcmBuffer() {
	// 1. If we already have enough data, we're done.
	if (pcmBuffer_.size() >= FFT_SIZE) {
		return true;
	}

	// 2. Collect new PCM blocks from the queue
	size_t buffered = pcmBuffer_.size();
	while (buffered < FFT_SIZE && !pcmQueue_.empty()) {
		std::vector<Uint8> pcmBlock;
		{
			std::lock_guard<std::mutex> lock(pcmMutex_);
			pcmBlock = std::move(pcmQueue_.front());
			pcmQueue_.pop_front();
		}

		// 3. Convert raw bytes to mono float samples and add to our buffer
		int sampleSize = musicPlayer_->getSampleSize();
		int channels = musicPlayer_->getAudioChannels();
		if (sampleSize <= 0 || channels <= 0) continue; // Safety check

		int numFrames = static_cast<int>(pcmBlock.size()) / (sampleSize * channels);
		for (int i = 0; i < numFrames && buffered < FFT_SIZE; ++i) {
			float sampleMono = 0.0f;
			for (int ch = 0; ch < channels; ++ch) {
				int pos = (i * channels + ch) * sampleSize;
				// Assuming S16LE format as before
				int16_t val = *reinterpret_cast<const int16_t*>(&pcmBlock[pos]);
				sampleMono += static_cast<float>(val) / 32768.0f;
			}
			sampleMono /= channels;
			pcmBuffer_.push_back(sampleMono);
			++buffered;
		}
	}

	// 4. Return whether we have enough data for a full FFT analysis
	return pcmBuffer_.size() >= FFT_SIZE;
}

bool MusicPlayerComponent::updateIsoFFT() {
	// If there's not enough data or FFT isn't configured, return an empty vector.
	if (!fillPcmBuffer() || !kissfft_cfg_) {
		return {};
	}

	// --- Step 1: Prepare Input and Perform Real FFT ---

	// Create a dedicated, clean buffer for this frame's FFT input.
	std::vector<float> fftInput(FFT_SIZE);
	std::copy(pcmBuffer_.begin(), pcmBuffer_.begin() + FFT_SIZE, fftInput.begin());

	// Calculate the mean of this specific window for DC offset removal.
	float mean = 0.0f;
	for (int i = 0; i < FFT_SIZE; ++i) mean += fftInput[i];
	mean /= FFT_SIZE;

	// Apply windowing and DC offset directly to the float input buffer.
	for (int i = 0; i < FFT_SIZE; ++i) {
		const float PI_F = 3.1415926535f;
		float window = 0.5f * (1.0f - cosf(2.0f * PI_F * static_cast<float>(i) / (FFT_SIZE - 1.0f)));
		fftInput[i] = (fftInput[i] - mean) * window;
	}

	// Perform the real-to-complex FFT.
	kiss_fftr(kissfft_cfg_, fftInput.data(), fftOutput_.data());

	// Update the main rolling buffer for the next frame.
	pcmBuffer_.erase(pcmBuffer_.begin(), pcmBuffer_.begin() + FFT_SIZE / 2);

	for (int i = 1; i < NR_OF_FREQ; ++i) {
		fftMagnitudes_[i] = sqrtf(fftOutput_[i].r * fftOutput_[i].r + fftOutput_[i].i * fftOutput_[i].i);
	}

	return true;
}

bool detect_beat_from_fft(const float* input, int nr_of_bins) {
	static std::vector<float> energy_buffer(128, 0.0f);
	static int energy_idx = 0;

	int drum_bin_start = 1;
	int drum_bin_end = nr_of_bins / 5;
	float current_energy = 0.0f;
	for (int j = drum_bin_start; j < drum_bin_end; ++j)
		current_energy += std::abs(input[j]);

	energy_buffer[energy_idx] = current_energy;
	energy_idx = (energy_idx + 1) % energy_buffer.size();

	const int window = 12;
	float avg = 0.0f;
	for (int k = 1; k <= window; ++k)
		avg += energy_buffer[(energy_idx - k + energy_buffer.size()) % energy_buffer.size()];
	avg /= window;

	return current_energy > avg * 1.4f && avg > 1.0f;
}

void HSVtoRGB(float h, float s, float v, Uint8& r, Uint8& g, Uint8& b) {
	float c = v * s;
	float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
	float m = v - c;
	float rp, gp, bp;
	if (h < 1.0f / 6.0f) { rp = c;  gp = x;  bp = 0; }
	else if (h < 2.0f / 6.0f) { rp = x;  gp = c;  bp = 0; }
	else if (h < 3.0f / 6.0f) { rp = 0;  gp = c;  bp = x; }
	else if (h < 4.0f / 6.0f) { rp = 0;  gp = x;  bp = c; }
	else if (h < 5.0f / 6.0f) { rp = x;  gp = 0;  bp = c; }
	else { rp = c;  gp = 0;  bp = x; }
	r = static_cast<Uint8>(std::clamp((rp + m) * 255.0f, 0.0f, 255.0f));
	g = static_cast<Uint8>(std::clamp((gp + m) * 255.0f, 0.0f, 255.0f));
	b = static_cast<Uint8>(std::clamp((bp + m) * 255.0f, 0.0f, 255.0f));
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

	if (isFftVisualizer() && renderer_ && fftTexture_) {
		bool needsRedraw = false;
		if (isIsoVisualizer_ && isoNeedsUpdate_) needsRedraw = true;
		if (isVuMeter_ && vuMeterNeedsUpdate_) needsRedraw = true;

		if (needsRedraw) {
			SDL_Texture* prevTarget = SDL_GetRenderTarget(renderer_);
			SDL_SetRenderTarget(renderer_, fftTexture_);

			// Select the correct drawing function
			if (isIsoVisualizer_) {
				SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
				SDL_RenderClear(renderer_);
				drawIsoVisualizer(renderer_, fftTexW_, fftTexH_);
				//isoNeedsUpdate_ = false; // Mark as drawn
			}
			else if (isVuMeter_) {
				drawVuMeterToTexture();
				//vuMeterNeedsUpdate_ = false; // Mark as drawn
			}

			SDL_SetRenderTarget(renderer_, prevTarget);
		}

		// Render the final texture to the screen
		SDL_FRect rect;
		rect.x = baseViewInfo.XRelativeToOrigin();
		rect.y = baseViewInfo.YRelativeToOrigin();
		rect.w = baseViewInfo.ScaledWidth();
		rect.h = baseViewInfo.ScaledHeight();
		SDL::renderCopyF(fftTexture_, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutWidthByMonitor(baseViewInfo.Monitor));
		return;
	}

	if (gstreamerVisType_ != GStreamerVisType::None) {
		if (!gstPipeline_) {
			createGstPipeline();
		}
		drawGstTexture();
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

void MusicPlayerComponent::drawVuMeterToTexture() {
	if (!renderer_ || !fftTexture_) return;

	// The target is already set to fftTexture_ by the draw() function.

	// Clear with transparent background (or the configured one)
	SDL_SetRenderDrawColor(renderer_, vuMeterConfig_.backgroundColor.r, vuMeterConfig_.backgroundColor.g, vuMeterConfig_.backgroundColor.b, vuMeterConfig_.backgroundColor.a);
	SDL_RenderClear(renderer_);
	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

	// Use fftTexW_ and fftTexH_ for dimensions
	int numChannels = vuMeterConfig_.isMono ? 1 : 2;
	float totalWidth = static_cast<float>(fftTexW_);
	float channelSpacing = totalWidth * 0.05f;
	float availableWidth = totalWidth - (channelSpacing * (numChannels - 1));
	float channelWidth = availableWidth / numChannels;

	for (int channel = 0; channel < numChannels; ++channel) {
		int barStart = vuMeterConfig_.isMono ? 0 : (channel == 0 ? 0 : vuMeterConfig_.barCount / 2);
		int barEnd = vuMeterConfig_.isMono ? vuMeterConfig_.barCount : (channel == 0 ? vuMeterConfig_.barCount / 2 : vuMeterConfig_.barCount);
		int barsInChannel = barEnd - barStart;
		if (barsInChannel <= 0) continue;

		float channelX = channel * (channelWidth + channelSpacing);
		float barWidth = channelWidth / static_cast<float>(barsInChannel);
		float barSpacing = barWidth * 0.2f;
		float actualBarWidth = barWidth - barSpacing;

		for (int i = 0; i < barsInChannel; i++) {
			int barIndex = barStart + i;
			float barX = channelX + static_cast<float>(i * barWidth) + (barSpacing / 2.0f);

			float barHeight = static_cast<float>(fftTexH_) * vuLevels_[barIndex];
			float peakHeight = static_cast<float>(fftTexH_) * vuPeaks_[barIndex];
			float greenZone = fftTexH_ * vuMeterConfig_.greenThreshold;
			float yellowZone = fftTexH_ * (vuMeterConfig_.yellowThreshold - vuMeterConfig_.greenThreshold);

			// Green
			if (barHeight > 0) {
				SDL_SetRenderDrawColor(renderer_, vuMeterConfig_.bottomColor.r, vuMeterConfig_.bottomColor.g, vuMeterConfig_.bottomColor.b, 255);
				float segmentHeight = std::min(barHeight, greenZone);
				SDL_FRect greenRect = { barX, fftTexH_ - segmentHeight, actualBarWidth, segmentHeight };
				SDL_RenderFillRectF(renderer_, &greenRect);
			}
			// Yellow
			if (barHeight > greenZone) {
				SDL_SetRenderDrawColor(renderer_, vuMeterConfig_.middleColor.r, vuMeterConfig_.middleColor.g, vuMeterConfig_.middleColor.b, 255);
				float segmentHeight = std::min(barHeight - greenZone, yellowZone);
				SDL_FRect yellowRect = { barX, fftTexH_ - greenZone - segmentHeight, actualBarWidth, segmentHeight };
				SDL_RenderFillRectF(renderer_, &yellowRect);
			}
			// Red
			if (barHeight > greenZone + yellowZone) {
				SDL_SetRenderDrawColor(renderer_, vuMeterConfig_.topColor.r, vuMeterConfig_.topColor.g, vuMeterConfig_.topColor.b, 255);
				float redSegmentHeight = barHeight - greenZone - yellowZone;
				// CORRECTED Y-COORDINATE: Start from the top of the bar.
				SDL_FRect redRect = { barX, fftTexH_ - barHeight, actualBarWidth, redSegmentHeight };
				SDL_RenderFillRectF(renderer_, &redRect);
			}
			// Peak marker
			if (peakHeight > 0 && peakHeight >= barHeight) {
				SDL_SetRenderDrawColor(renderer_, vuMeterConfig_.peakColor.r, vuMeterConfig_.peakColor.g, vuMeterConfig_.peakColor.b, 255);
				SDL_FRect peakRect = { barX, fftTexH_ - peakHeight - 2, actualBarWidth, 2.0f };
				SDL_RenderFillRectF(renderer_, &peakRect);
			}
		}
	}
}

void MusicPlayerComponent::drawIsoVisualizer(SDL_Renderer* renderer, int win_w, int win_h) {
	if (iso_grid_.empty() || win_w <= 0 || win_h <= 0) return;

	// --- 1. Define Scale Factors and Tweakable Constants ---
	const float ref_w = 1280.0f;
	const float ref_h = 720.0f;
	const float scale_w = float(win_w) / ref_w;
	const float scale_h = float(win_h) / ref_h;

	// --- Vignette & Color Controls ---
	const float vignette_power = 2.0f;
	const float vignette_radius = 1.0f;
	const float arch_factor = 0.0005f;

	// --- Peak Color Controls ---
	const float BASE_HUE = 120.0f / 360.0f; // Green
	const float PEAK_HUE = 0.0f / 360.0f;   // Red
	const float MIN_BRIGHTNESS = 0.4f;      // Brightness of valleys (0.0 to 1.0)
	const float MAX_Z_FOR_COLOR = 50.0f;    // The Z-height that corresponds to peak color/brightness

	// --- 2. Animation and Geometry Constants ---
	float eased_offset = 0.5f * (1.0f - cosf(iso_scroll_offset_ * float(M_PI)));
	const int tall = ISO_HISTORY;
	const int nr = NR_OF_FREQ;
	const float inc = 0.7f;
	const float yOffset = float(win_h) * 0.7f;

	// The grid stores the final 2D point AND its Z-amplitude for coloring.
	struct PointData { SDL_FPoint pos; float z_amp; };
	std::vector<std::vector<PointData>> td_grid(tall, std::vector<PointData>(nr));

	// --- 3. Projection Loop: Calculate all point data first ---
	for (int i = 0; i < tall - 1; ++i) {
		float arch_term = arch_factor * float(i) * float(i);
		float i_over_10 = float(i) / 10.0f;

		for (int j = 0; j < nr; ++j) {
			float z_amplitude = iso_grid_[i][j].z * (1.0f - eased_offset) + iso_grid_[i + 1][j].z * eased_offset;
			td_grid[i][j].z_amp = z_amplitude;

			float static_x = iso_grid_[i][j].x;
			float static_y = iso_grid_[i][j].y;
			float local_x = inc * static_x * (8.0f / 10.0f + static_y * static_y * 0.0001f);
			float local_y = arch_term * static_y - z_amplitude - z_amplitude * i_over_10 * inc;

			td_grid[i][j].pos.x = local_x * scale_w + float(win_w) / 2.0f;
			td_grid[i][j].pos.y = local_y * scale_h + yOffset;
		}
	}

	// --- 4. Drawing Loop with Per-Line Vignette and Color Modulation ---
	const float centerX = float(win_w) / 2.0f;
	const float centerY = float(win_h) / 2.0f;

	const float centerX_div = centerX * vignette_radius;
	const float centerY_div = centerY * vignette_radius;

	for (int i = 1; i < tall - 2; ++i) {
		for (int j = 1; j < nr; ++j) {
			// --- Horizontal Line ---
			PointData p1_h = td_grid[i][j - 1];
			PointData p2_h = td_grid[i][j];

			// VIGNETTE calculation
			float mid_x_h = (p1_h.pos.x + p2_h.pos.x) / 2.0f;
			float mid_y_h = (p1_h.pos.y + p2_h.pos.y) / 2.0f;
			float dx_h = (mid_x_h - centerX) / centerX_div;
			float dy_h = (mid_y_h - centerY) / centerY_div;
			float distance_h = std::clamp(std::sqrt(dx_h * dx_h + dy_h * dy_h), 0.0f, 1.0f);
			float vignette_fade = 1.0f - std::pow(distance_h, vignette_power);

			// PEAK COLOR calculation
			float avg_z_h = (p1_h.z_amp + p2_h.z_amp) / 2.0f;
			float peak_mix = std::clamp(avg_z_h / MAX_Z_FOR_COLOR, 0.0f, 1.0f);
			float hue_h = BASE_HUE * (1.0f - peak_mix) + PEAK_HUE * peak_mix;
			float value_h = MIN_BRIGHTNESS * (1.0f - peak_mix) + 1.0f * peak_mix;

			// Combine vignette and peak brightness
			Uint8 r_h, g_h, b_h;
			HSVtoRGB(hue_h, 1.0f, value_h * vignette_fade, r_h, g_h, b_h);

			SDL_SetRenderDrawColor(renderer, r_h, g_h, b_h, 255);
			SDL_RenderDrawLineF(renderer, p1_h.pos.x, p1_h.pos.y, p2_h.pos.x, p2_h.pos.y);

			// --- Vertical Line ---
			PointData p1_v = td_grid[i - 1][j];
			PointData p2_v = td_grid[i][j];

			float mid_x_v = (p1_v.pos.x + p2_v.pos.x) / 2.0f;
			float mid_y_v = (p1_v.pos.y + p2_v.pos.y) / 2.0f;
			float dx_v = (mid_x_v - centerX) / (centerX * vignette_radius);
			float dy_v = (mid_y_v - centerY) / (centerY * vignette_radius);
			float distance_v = std::clamp(std::sqrt(dx_v * dx_v + dy_v * dy_v), 0.0f, 1.0f);
			float vignette_fade_v = 1.0f - std::pow(distance_v, vignette_power);

			float avg_z_v = (p1_v.z_amp + p2_v.z_amp) / 2.0f;
			float peak_mix_v = std::clamp(avg_z_v / MAX_Z_FOR_COLOR, 0.0f, 1.0f);
			float hue_v = BASE_HUE * (1.0f - peak_mix_v) + PEAK_HUE * peak_mix_v;
			float value_v = MIN_BRIGHTNESS * (1.0f - peak_mix_v) + 1.0f * peak_mix_v;

			Uint8 r_v, g_v, b_v;
			HSVtoRGB(hue_v, 1.0f, value_v * vignette_fade_v, r_v, g_v, b_v);

			SDL_SetRenderDrawColor(renderer, r_v, g_v, b_v, 255);
			SDL_RenderDrawLineF(renderer, p1_v.pos.x, p1_v.pos.y, p2_v.pos.x, p2_v.pos.y);
		}
	}
}

void MusicPlayerComponent::createGstPipeline() {
	const char* visElementName = nullptr;
	const char* visElementNick = nullptr;

	switch (gstreamerVisType_) {
		case GStreamerVisType::Goom:
		visElementName = "goom";
		visElementNick = "goom";
		break;
		case GStreamerVisType::Wavescope:
		visElementName = "wavescope";
		visElementNick = "wavescope";
		break;
		case GStreamerVisType::Synaescope:
		visElementName = "synaescope";
		visElementNick = "synaescope";
		break;
		case GStreamerVisType::Spectrascope:
		visElementName = "spectrascope";
		visElementNick = "spectrascope";
		break;
		default:
		LOG_ERROR("MusicPlayerComponent", "Invalid or missing visualizer type");
		return;
	}

	gstPipeline_ = gst_pipeline_new("vizualizer-pipeline");
	gstAppSrc_ = gst_element_factory_make("appsrc", "audio-input");
	GstElement* convert = gst_element_factory_make("audioconvert", "convert");
	GstElement* resample = gst_element_factory_make("audioresample", "resample");
	GstElement* visualizer = gst_element_factory_make(visElementName, visElementNick);
	GstElement* vconvert = gst_element_factory_make("videoconvert", "vconvert");
	gstAppSink_ = gst_element_factory_make("appsink", "video-output");

	if (!gstPipeline_ || !gstAppSrc_ || !convert || !visualizer || !vconvert || !gstAppSink_) {
		LOG_ERROR("MusicPlayerComponent", "Failed to create goom pipeline elements");
		return;
	}

	// 2. Add & link
	gst_bin_add_many(GST_BIN(gstPipeline_), gstAppSrc_, convert, resample, visualizer, vconvert, gstAppSink_, NULL);
	if (!gst_element_link_many(gstAppSrc_, convert, resample, visualizer, vconvert, gstAppSink_, NULL)) {
		LOG_ERROR("MusicPlayerComponent", "Failed to link goom pipeline elements");
		return;
	}

	// 3. Set audio caps for appsrc
	GstCaps* audio_caps = gst_caps_new_simple("audio/x-raw",
		"format", G_TYPE_STRING, "S16LE",
		"rate", G_TYPE_INT, 44100,
		"channels", G_TYPE_INT, 2,
		"layout", G_TYPE_STRING, "interleaved",
		NULL);
	g_object_set(gstAppSrc_, "caps", audio_caps,
		"stream-type", 0 /* GST_APP_STREAM_TYPE_STREAM */,
		"format", GST_FORMAT_TIME,
		"is-live", TRUE,
		NULL);
	gst_caps_unref(audio_caps);

	int width = static_cast<int>(baseViewInfo.ScaledWidth());
	int height = static_cast<int>(baseViewInfo.ScaledHeight());

	// 4. Set appsink caps (video)
	GstCaps* video_caps = gst_caps_new_simple("video/x-raw",
		"format", G_TYPE_STRING, "RGB",
		"width", G_TYPE_INT, width,
		"height", G_TYPE_INT, height,
		"framerate", GST_TYPE_FRACTION, 60, 1, // <-- 60 fps
		NULL);
	if (gstreamerVisType_ == GStreamerVisType::Wavescope) {
		g_object_set(visualizer, "style", 3, NULL); // 1 for default mode
	}
	
	g_object_set(gstAppSink_, "caps", video_caps,
		"emit-signals", FALSE,
		"sync", FALSE,
		"max-buffers", 2,
		"drop", TRUE,
		NULL);
	gst_caps_unref(video_caps);

	// 7. Start pipeline in PAUSED, then PLAYING (optional)
	//gst_element_set_state(gstPipeline_, GST_STATE_PAUSED);
	gst_element_set_state(gstPipeline_, GST_STATE_PLAYING);
}

void MusicPlayerComponent::updateGstTextureFromAppSink() {
	if (!gstAppSink_)
		return;

	GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(gstAppSink_), 0); // non-blocking
	if (!sample)
		return; // No new frame

	GstCaps* caps = gst_sample_get_caps(sample);
	GstStructure* s = gst_caps_get_structure(caps, 0);
	int width = 0, height = 0;
	gst_structure_get_int(s, "width", &width);
	gst_structure_get_int(s, "height", &height);

	GstBuffer* buffer = gst_sample_get_buffer(sample);
	GstMapInfo map;
	if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
		if (!gstTexture_ || gstTexW_ != width || gstTexH_ != height) {
			if (gstTexture_) SDL_DestroyTexture(gstTexture_);
			gstTexture_ = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor),
				SDL_PIXELFORMAT_RGB24,
				SDL_TEXTUREACCESS_STREAMING,
				width, height);
			SDL_SetTextureBlendMode(gstTexture_, softOverlayBlendMode);
			gstTexW_ = width;
			gstTexH_ = height;
		}
		// We'll make a copy to tint before uploading
		std::vector<uint8_t> tintedData(width * height * 3);
		uint8_t* src = map.data;
		uint8_t* dst = tintedData.data();

		for (int y = 0; y < height; ++y) {
			float pos = float(y) / float(height - 1); // 0.0 at top, 1.0 at bottom
			uint8_t r, g, b;

			if (pos < 0.5f) {
				// Top half: red -> yellow
				float t = pos / 0.7f; // 0.0 to 1.0
				r = 255;
				g = uint8_t(255 * t);
				b = 0;
			}
			else {
				// Bottom half: yellow -> green
				float t = (pos - 0.5f) / 0.5f; // 0.0 to 1.0
				r = uint8_t(255 * (1.0f - t));
				g = 255;
				b = 0;
			}

			for (int x = 0; x < width; ++x) {
				int i = (y * width + x) * 3;
				uint8_t srcVal = src[i]; // Since the bar output is white, all channels are same (RGB)

				// Apply color by scaling: srcVal/255.0 * gradient
				dst[i + 0] = uint8_t((srcVal * r) / 255);
				dst[i + 1] = uint8_t((srcVal * g) / 255);
				dst[i + 2] = uint8_t((srcVal * b) / 255);
			}
		}

		// You can replace the Lock/Unlock section with UpdateTexture:
		SDL_UpdateTexture(gstTexture_, nullptr, map.data, width * 3);
		gst_buffer_unmap(buffer, &map);
	}
	gst_sample_unref(sample);
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

void MusicPlayerComponent::drawGstTexture() {
	if (!renderer_ || !gstTexture_ || baseViewInfo.Alpha <= 0.0f)
		return;

	SDL_FRect rect;
	rect.x = baseViewInfo.XRelativeToOrigin();
	rect.y = baseViewInfo.YRelativeToOrigin();
	rect.w = baseViewInfo.ScaledWidth();
	rect.h = baseViewInfo.ScaledHeight();

	SDL::renderCopyF(
		gstTexture_,
		baseViewInfo.Alpha,
		nullptr, // Full source texture
		&rect,
		baseViewInfo,
		page.getLayoutWidth(baseViewInfo.Monitor),
		page.getLayoutHeight(baseViewInfo.Monitor)
	);
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
	if (isAlbumArt_ || isVolumeBar_ || isProgressBar_ || gstreamerVisType_ != GStreamerVisType::None || isFftVisualizer_ || !musicPlayer_->hasStartedPlaying()) {
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
				//loadedComponent_->freeGraphicsMemory();
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
			Image* oldImage = dynamic_cast<Image*>(loadedComponent_);
			Image* newPotentialImage = dynamic_cast<Image*>(newImageComponent);
			bool pathChanged = true;
			if (oldImage && newPotentialImage && oldImage->filePath() == newPotentialImage->filePath()) {
				pathChanged = false;
			}

			if (pathChanged) {
				if (loadedComponent_) {
					//loadedComponent_->freeGraphicsMemory();
					delete loadedComponent_;
				}
				loadedComponent_ = newImageComponent;
				loadedComponent_->allocateGraphicsMemory(); // Essential for new images
			}
			else {
				// Paths are the same, no need to replace. Delete the newly created one.
				//newImageComponent->freeGraphicsMemory(); // Or just delete if it cleans up
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
			//loadedComponent_->freeGraphicsMemory();
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