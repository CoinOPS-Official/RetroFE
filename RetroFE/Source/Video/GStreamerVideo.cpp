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
#include "GStreamerVideo.h"
#include "../Database/Configuration.h"
#include "../Graphics/Component/Image.h"
#include "../Graphics/ViewInfo.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gst/audio/audio.h>
#include <gst/gstdebugutils.h>
#include <gst/video/video.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

bool GStreamerVideo::initialized_ = false;

GStreamerVideo::GStreamerVideo(int monitor)

	: monitor_(monitor)

{
	//gst_video_info_init(&videoInfo_);
	videoBuffer_.store(nullptr); // Initialize the atomic pointer
}

GStreamerVideo::~GStreamerVideo() { GStreamerVideo::stop(); }

void GStreamerVideo::setNumLoops(int n) {
	if (n > 0)
		numLoops_ = n;
}

SDL_Texture* GStreamerVideo::getTexture() const { return texture_; }

bool GStreamerVideo::initialize() {
	if (initialized_) {
		initialized_ = true;
		return true;
	}
	if (!gst_is_initialized()) {
		LOG_DEBUG("GStreamer", "Initializing in instance");
		gst_init(nullptr, nullptr);
		std::string path =
			Utils::combinePath(Configuration::absolutePath, "retrofe");
#ifdef WIN32
		GstRegistry* registry = gst_registry_get();
		gst_registry_scan_path(registry, path.c_str());
#endif
	}
	initialized_ = true;
	return true;
}

bool GStreamerVideo::deInitialize() {
	gst_deinit();
	initialized_ = false;
	paused_ = false;
	return true;
}

bool GStreamerVideo::stop() {
	if (!initialized_) {
		return false;
	}
	isPlaying_ = false;
	// Disable handoffs for videoSink
	if (videoSink_) {
		g_object_set(G_OBJECT(videoSink_), "signal-handoffs", FALSE, nullptr);
	}
	// Disconnect associated signals
	if (playbin_ && elementSetupHandlerId_) {
		g_signal_handler_disconnect(playbin_, elementSetupHandlerId_);
		elementSetupHandlerId_ = 0;
	}
	if (videoSink_ && handoffHandlerId_) {
		g_signal_handler_disconnect(videoSink_, handoffHandlerId_);
		handoffHandlerId_ = 0;
	}
	// Initiate the transition of playbin to GST_STATE_NULL without waiting
	if (playbin_) {
		gst_element_set_state(playbin_, GST_STATE_NULL);

		// Optionally perform a quick, non-blocking state check
		GstStateChangeReturn ret =
			gst_element_get_state(playbin_, nullptr, nullptr, 0);
		if (ret != GST_STATE_CHANGE_SUCCESS && ret != GST_STATE_CHANGE_ASYNC) {
			LOG_ERROR("Video",
				"Unexpected state change result when stopping playback");
		}
	}
	// Release the custom video sink bin
	if (videoBin_) {
		gst_object_unref(videoBin_);
		videoBin_ = nullptr;
	}
	// Release SDL Texture
	if (texture_) {
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	// Unref the video buffer
	if (GstBuffer* localBuffer = videoBuffer_.exchange(nullptr)) {
		gst_clear_buffer(&localBuffer);
	}
	// Free GStreamer elements and related resources
	if (playbin_) {
		gst_object_unref(GST_OBJECT(playbin_));
		playbin_ = nullptr;
	}
	// Reset remaining pointers and variables to ensure the object is in a clean
	// state.
	videoBus_ = nullptr;
	playbin_ = nullptr;
	videoBin_ = nullptr;
	capsFilter_ = nullptr;
	videoSink_ = nullptr;
	videoBuffer_ = nullptr;
	height_ = 0;
	width_ = 0;
	return true;
}

bool GStreamerVideo::play(const std::string& file) {
	playCount_ = 0;
	if (!initialized_)
		return false;
#if defined(WIN32)
	enablePlugin("directsoundsink");
	if (!Configuration::HardwareVideoAccel) {
		//enablePlugin("openh264dec");
		disablePlugin("d3d11h264dec");
		disablePlugin("d3d11h265dec");
	}
#elif defined(__APPLE__)
	// if (Configuration::HardwareVideoAccel) {
	//     enablePlugin("vah264dec");
	//     enablePlugin("vah265dec");
	// }
#else
	if (Configuration::HardwareVideoAccel) {
		enablePlugin("vah264dec");
		enablePlugin("vah265dec");
	}
	if (!Configuration::HardwareVideoAccel) {
		disablePlugin("vah264dec");
		disablePlugin("vah265dec");
		enablePlugin("avdec_h264");
		enablePlugin("avdec_h265");
	}
#endif
	currentFile_ = file;
	if (!initializeGstElements(file))
		return false;
	// Start playing
	if (GstStateChangeReturn playState =
		gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING);
		playState != GST_STATE_CHANGE_ASYNC) {
		isPlaying_ = false;
		LOG_ERROR("Video", "Unable to set the pipeline to the playing state.");
		stop();
		return false;
	}
	paused_ = false;
	isPlaying_ = true;
	// Set the volume to zero and mute the video
	gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_),
		GST_STREAM_VOLUME_FORMAT_LINEAR, 0.0);
	gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), true);

	if (Configuration::debugDotEnabled) {
		// Environment variable is set, proceed with dot file generation
		GstState state;
		GstState pending;
		// Wait up to 5 seconds for the state change to complete
		GstClockTime timeout = 5 * GST_SECOND; // Define your timeout
		GstStateChangeReturn ret =
			gst_element_get_state(GST_ELEMENT(playbin_), &state, &pending, timeout);
		if (ret == GST_STATE_CHANGE_SUCCESS && state == GST_STATE_PLAYING) {
			// The pipeline is in the playing state, proceed with dot file generation
			// Generate dot file for playbin_
			std::string playbinDotFileName =
				generateDotFileName("playbin", currentFile_);
			GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(playbin_), GST_DEBUG_GRAPH_SHOW_ALL,
				playbinDotFileName.c_str());
			// Generate dot file for videoBin_
			std::string videoBinDotFileName =
				generateDotFileName("videobin", currentFile_);
			GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(videoBin_), GST_DEBUG_GRAPH_SHOW_ALL,
				videoBinDotFileName.c_str());
		}
	}
	return true;
}

bool GStreamerVideo::initializeGstElements(const std::string& file) {
	gchar* uriFile = gst_filename_to_uri(file.c_str(), nullptr);
	if (!uriFile)
		return false;
	playbin_ = gst_element_factory_make("playbin3", "player");
	videoBin_ = gst_bin_new("SinkBin");
	videoSink_ = gst_element_factory_make("fakesink", "video_sink");
	capsFilter_ = gst_element_factory_make("capsfilter", "caps_filter");
	GstCaps* videoConvertCaps;
	if (Configuration::HardwareVideoAccel) {
		videoConvertCaps = gst_caps_from_string(
			"video/x-raw,format=(string)NV12,pixel-aspect-ratio=(fraction)1/1");
	}
	else {
		videoConvertCaps = gst_caps_from_string(
			"video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1");
	}
	if (!playbin_ || !videoSink_ || !capsFilter_) {
		LOG_DEBUG("Video", "Could not create elements");
		return false;
	}
	g_object_set(G_OBJECT(capsFilter_), "caps", videoConvertCaps, nullptr);
	gst_caps_unref(videoConvertCaps);
	videoConvertCaps = nullptr;
	gst_bin_add_many(GST_BIN(videoBin_), capsFilter_, videoSink_, nullptr);
	// Directly link capsFilter to videoSink
	if (!gst_element_link(capsFilter_, videoSink_)) {
		LOG_DEBUG("Video", "Could not link video processing elements");
		return false;
	}
	GstPad* sinkPad = nullptr;
	sinkPad = gst_element_get_static_pad(capsFilter_, "sink");
	GstPad* ghostPad = gst_ghost_pad_new("sink", sinkPad);
	gst_element_add_pad(videoBin_, ghostPad);
	gst_object_unref(sinkPad);
	// Set properties of playbin and videoSink
	const guint PLAYBIN_FLAGS = 0x00000001 | 0x00000002;
	g_object_set(G_OBJECT(playbin_), "uri", uriFile, "video-sink", videoBin_,
		"instant-uri", TRUE, "flags", PLAYBIN_FLAGS, "buffer-size", -1,
		nullptr);
	g_free(uriFile);
	elementSetupHandlerId_ = g_signal_connect(
		playbin_, "element-setup", G_CALLBACK(elementSetupCallback), this);
	videoBus_ = gst_pipeline_get_bus(GST_PIPELINE(playbin_));
	gst_object_unref(videoBus_);
	g_object_set(G_OBJECT(videoSink_), "signal-handoffs", TRUE, "sync", TRUE,
		"enable-last-sample", FALSE, nullptr);
	handoffHandlerId_ = g_signal_connect(videoSink_, "handoff",
		G_CALLBACK(processNewBuffer), this);

	return true;
}

void GStreamerVideo::elementSetupCallback(
	[[maybe_unused]] GstElement const* playbin, GstElement* element,
	[[maybe_unused]] GStreamerVideo const* video) {
	gchar* elementName = gst_element_get_name(element);
	if (!Configuration::HardwareVideoAccel) {
		if (g_str_has_prefix(elementName, "avdec_h26")) {
			// Modify the properties of the avdec_h265 element here
			g_object_set(G_OBJECT(element), "thread-type",
				Configuration::AvdecThreadType, "max-threads",
				Configuration::AvdecMaxThreads, "direct-rendering", false,
				nullptr);
		}
	}
#ifdef WIN32
	if (strstr(elementName, "wasapi2") != nullptr) {
		g_object_set(G_OBJECT(element), "low-latency", TRUE, nullptr);
	}
#endif
	g_free(elementName);
}

void GStreamerVideo::processNewBuffer(GstElement const* /* fakesink */, const GstBuffer* buf, GstPad* new_pad, gpointer userdata) {
	auto* video = static_cast<GStreamerVideo*>(userdata);
	if (video && video->isPlaying_) {
		// Retrieve caps and set video info if not yet set.
		if (!video->width_ || !video->height_) {
			if (GstCaps* caps = gst_pad_get_current_caps(new_pad)) {
				if (gst_video_info_from_caps(&video->videoInfo_, caps)) {
					video->width_ = video->videoInfo_.width;
					video->height_ = video->videoInfo_.height;
					video->videoFormat_ = GST_VIDEO_INFO_FORMAT(&video->videoInfo_);

					if (video->videoFormat_ == GST_VIDEO_FORMAT_NV12) {
						LOG_INFO("Video", "Pixel Format : NV12");
					}
					else if (video->videoFormat_ == GST_VIDEO_FORMAT_I420) {
						LOG_INFO("Video", "Pixel Format : I420");
					}
					else {
						LOG_ERROR("Video", "Unsupported pixel format.");
					}

					// Create texture now that width, height, and pixel format are known
					if (video->height_ && video->width_) {
						SDL_PixelFormatEnum sdlFormat = (video->videoFormat_ == GST_VIDEO_FORMAT_NV12) ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
						video->texture_ = SDL_CreateTexture(SDL::getRenderer(video->monitor_), sdlFormat, SDL_TEXTUREACCESS_STREAMING, video->width_, video->height_);
						SDL_SetTextureBlendMode(video->texture_, SDL_BLENDMODE_BLEND);
					}
				}
				else {
					LOG_ERROR("Video", "Failed to set video info from caps.");
				}
				gst_caps_unref(caps);
			}
		}
		// Clear the existing videoBuffer_ if it exists and set the new buffer.
		if (GstBuffer* oldBuffer = video->videoBuffer_.exchange(gst_buffer_copy(buf))) {
			gst_clear_buffer(&oldBuffer);
		}
		video->frameReady_ = true;
	}
}

void GStreamerVideo::update(float /* dt */) {
	if (!playbin_ || !texture_ || paused_) {
		return;
	}

	GstBuffer* localBuffer = videoBuffer_.exchange(nullptr);
	if (!localBuffer) {
		return;
	}

	GstVideoFrame vframe;
	if (gst_video_frame_map(&vframe, &videoInfo_, localBuffer, GST_MAP_READ)) {
		SDL_LockMutex(SDL::getMutex());

		if (videoFormat_ == GST_VIDEO_FORMAT_NV12) {
			if (SDL_UpdateNVTexture(texture_, nullptr,
				static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0),
				static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1)) != 0) {
				LOG_ERROR("Video", "SDL_UpdateNVTexture failed: " + std::string(SDL_GetError()));
			}
		}
		else if (videoFormat_ == GST_VIDEO_FORMAT_I420) {
			if (SDL_UpdateYUVTexture(texture_, nullptr,
				static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0),
				static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1),
				static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 2)),
				GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 2)) != 0) {
				LOG_ERROR("Video", "SDL_UpdateYUVTexture failed: " + std::string(SDL_GetError()));
			}
		}
		else {
			LOG_ERROR("Video", "Unsupported format or fallback handling required.");
		}

		SDL_UnlockMutex(SDL::getMutex());
		gst_video_frame_unmap(&vframe);

		gst_clear_buffer(&localBuffer);
	}
	else {
		LOG_ERROR("Video", "Failed to map video buffer.");
	}
}

void GStreamerVideo::loopHandler() {
	if (videoBus_) {
		GstMessage* msg = gst_bus_pop_filtered(videoBus_, GST_MESSAGE_EOS);
		if (msg) {
			playCount_++;
			// If the number of loops is 0 or greater than the current playCount_,
			// seek the playback to the beginning.
			if (!numLoops_ || numLoops_ > playCount_) {
				gst_element_seek(playbin_, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
					GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE,
					GST_CLOCK_TIME_NONE);
			}
			else {
				stop();
			}
			gst_message_unref(msg);
		}
	}
}

void GStreamerVideo::volumeUpdate() {
	bool shouldMute = false;
	double targetVolume = 0.0;
	if (bool muteVideo = Configuration::MuteVideo; muteVideo) {
		shouldMute = true;
	}
	else {
		if (volume_ > 1.0)
			volume_ = 1.0;
		if (currentVolume_ > volume_ || currentVolume_ + 0.005 >= volume_)
			currentVolume_ = volume_;
		else
			currentVolume_ += 0.005;
		targetVolume = currentVolume_;
		if (currentVolume_ < 0.1)
			shouldMute = true;
	}
	// Only set the volume if it has changed since the last call.
	if (targetVolume != lastSetVolume_) {
		gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_),
			GST_STREAM_VOLUME_FORMAT_LINEAR, targetVolume);
		lastSetVolume_ = targetVolume;
	}
	// Only set the mute state if it has changed since the last call.
	if (shouldMute != lastSetMuteState_) {
		gst_stream_volume_set_mute(GST_STREAM_VOLUME(playbin_), shouldMute);
		lastSetMuteState_ = shouldMute;
	}
}

int GStreamerVideo::getHeight() { return height_; }

int GStreamerVideo::getWidth() { return width_; }

void GStreamerVideo::draw() { frameReady_ = false; }

bool GStreamerVideo::isPlaying() { return isPlaying_; }

void GStreamerVideo::setVolume(float volume) { volume_ = volume; }

void GStreamerVideo::skipForward() {
	if (!isPlaying_)
		return;
	gint64 current;
	gint64 duration;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration))
		return;
	current += 60 * GST_SECOND;
	if (current > duration)
		current = duration - 1;
	gst_element_seek_simple(
		playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), current);
}

void GStreamerVideo::skipBackward() {
	if (!isPlaying_)
		return;
	gint64 current;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (current > 60 * GST_SECOND)
		current -= 60 * GST_SECOND;
	else
		current = 0;
	gst_element_seek_simple(
		playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), current);
}

void GStreamerVideo::skipForwardp() {

	if (!isPlaying_)
		return;
	gint64 current;
	gint64 duration;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration))
		return;
	current += duration / 20;
	if (current > duration)
		current = duration - 1;
	gst_element_seek_simple(
		playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), current);
}

void GStreamerVideo::skipBackwardp() {

	if (!isPlaying_)
		return;

	gint64 current;
	gint64 duration;

	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current))
		return;

	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration))
		return;

	if (current > duration / 20)
		current -= duration / 20;
	else
		current = 0;
	gst_element_seek_simple(
		playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), current);
}

void GStreamerVideo::pause() {
	paused_ = !paused_;
	if (paused_) {
		gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PAUSED);
	}
	else {
		gst_element_set_state(GST_ELEMENT(playbin_), GST_STATE_PLAYING);
	}
}

void GStreamerVideo::restart() {
	if (!isPlaying_)
		return;
	gst_element_seek_simple(playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH), 0);
}


unsigned long long GStreamerVideo::getCurrent() {
	gint64 ret = 0;
	if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &ret) || !isPlaying_)
		ret = 0;
	return (unsigned long long)ret;
}

unsigned long long GStreamerVideo::getDuration() {
	gint64 ret = 0;
	if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &ret) ||
		!isPlaying_)
		ret = 0;
	return (unsigned long long)ret;
}

bool GStreamerVideo::isPaused() { return paused_; }

bool GStreamerVideo::getFrameReady() { return frameReady_; }

std::string GStreamerVideo::generateDotFileName(const std::string& prefix,
	const std::string& videoFilePath) const {
	std::string videoFileName = Utils::getFileName(videoFilePath);

	auto now = std::chrono::system_clock::now();
	auto now_c = std::chrono::system_clock::to_time_t(now);
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()) %
		1000000;

	std::stringstream ss;
	ss << prefix << "_" << videoFileName << "_"
		<< std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S_")
		<< std::setfill('0') << std::setw(6) << microseconds.count();

	return ss.str();
}

void GStreamerVideo::enablePlugin(const std::string& pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory) {
		// Sets the plugin rank to PRIMARY + 1 to prioritize its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory),
			GST_RANK_PRIMARY + 1);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::disablePlugin(const std::string& pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory) {
		// Sets the plugin rank to GST_RANK_NONE to disable its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
		gst_object_unref(factory);
	}
}
