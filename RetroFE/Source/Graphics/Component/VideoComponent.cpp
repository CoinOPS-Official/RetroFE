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

#include "VideoComponent.h"

#include <string>
#include <type_traits>
#include <utility>

#include "../../Video/GStreamerVideo.h"

#include "../../Graphics/ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../Video/IVideo.h"
#include "../../Video/VideoFactory.h"
#include "../Page.h"
#include "SDL_rect.h"
#include "SDL_render.h"
#include <gst/video/video.h>


VideoComponent::VideoComponent(Page &p, const std::string &videoFile, int monitor, int numLoops, bool softOverlay)
    : Component(p), videoFile_(videoFile), numLoops_(numLoops), monitor_(monitor), currentPage_(&p), softOverlay_(softOverlay)
{
}

VideoComponent::~VideoComponent()
{
    VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt)
{
    if (!videoInst_ || !videoInst_->isPlaying())
    {
        return Component::update(dt);
    }

    else
    {
        videoInst_->setVolume(baseViewInfo.Volume);
        // videoInst_->update(dt);
        if (!currentPage_->isMenuScrolling())
        {
            videoInst_->volumeUpdate();
            videoInst_->loopHandler();
        }

        if (baseViewInfo.ImageHeight == 0 && baseViewInfo.ImageWidth == 0)
        {
            baseViewInfo.ImageHeight = static_cast<float>(videoInst_->getHeight());
            baseViewInfo.ImageWidth = static_cast<float>(videoInst_->getWidth());
        }

        bool isCurrentlyVisible = baseViewInfo.Alpha > 0.0f;

        if (isCurrentlyVisible)
        {       
            hasBeenOnScreen_ = true;
            if (videoInst_->isBufferDisconnected())
                videoInst_->bufferDisconnect(false);
        }
        else
        {
            if (!videoInst_->isBufferDisconnected())
                videoInst_->bufferDisconnect(true);
        }

        if (currentPage_->isMenuFastScrolling() && videoInst_->isBufferDisconnected())
            videoInst_->bufferDisconnect(false);

        if (baseViewInfo.PauseOnScroll)
        {
            if (!isCurrentlyVisible && !videoInst_->isPaused() && !currentPage_->isMenuFastScrolling())
            {
                videoInst_->pause();
                if (Logger::isLevelEnabled("DEBUG"))
                    LOG_DEBUG("VideoComponent", "Paused " + Utils::getFileName(videoFile_));
            }
            else if (isCurrentlyVisible && videoInst_->isPaused())
            {
                videoInst_->pause();
                if (Logger::isLevelEnabled("DEBUG"))
                    LOG_DEBUG("VideoComponent", "Resumed " + Utils::getFileName(videoFile_));
            }
        }

        if (baseViewInfo.Restart && hasBeenOnScreen_)
        {
            if (videoInst_->isPaused())
                videoInst_->pause();
            videoInst_->restart();
            baseViewInfo.Restart = false;
            if (Logger::isLevelEnabled("DEBUG"))
                LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
        }
    }

    return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory()
{
    Component::allocateGraphicsMemory();

    if (!isPlaying_) {
        if (!videoInst_ && videoFile_ != "") {
            videoInst_ = VideoFactory::createVideo(monitor_, numLoops_, softOverlay_);
            isPlaying_ = videoInst_->play(videoFile_);
        }
    }
}

void VideoComponent::freeGraphicsMemory()
{
    Component::freeGraphicsMemory();
    if (Logger::isLevelEnabled("DEBUG"))
        LOG_DEBUG("VideoComponent", "Component Freed " + Utils::getFileName(videoFile_));

    if (videoInst_) {
        videoInst_->stop();
        delete videoInst_;
        isPlaying_ = false;
        if (Logger::isLevelEnabled("DEBUG"))
            LOG_DEBUG("VideoComponent", "Deleted " + Utils::getFileName(videoFile_));
        videoInst_ = nullptr;

    }
}

void VideoComponent::draw() {
    if (videoInst_) {
        if (videoInst_->isPlaying() && isPlaying_) {
            videoInst_->draw();
        }
        if (SDL_Texture* texture = videoInst_->getTexture()) {
            SDL_Rect rect = {
                static_cast<int>(baseViewInfo.XRelativeToOrigin()), static_cast<int>(baseViewInfo.YRelativeToOrigin()),
                static_cast<int>(baseViewInfo.ScaledWidth()), static_cast<int>(baseViewInfo.ScaledHeight()) };

            LOG_DEBUG("VideoComponent", "Drawing texture...");
            SDL::renderCopy(texture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
                page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
                page.getLayoutHeightByMonitor(baseViewInfo.Monitor));

            if (videoInst_->isNewFrameAvailable()) {
                // Get the current clock and base time
                GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(videoInst_->getPipeline()));
                GstClockTime currentTime = gst_clock_get_time(clock);
                GstClockTime baseTime = gst_element_get_base_time(GST_ELEMENT(videoInst_->getPipeline()));
                gst_object_unref(clock);

                // Calculate the actual presentation time based on the base time and PTS
                GstClockTime pts = videoInst_->getLastPTS();
                GstClockTime expectedTime = baseTime + pts;

                // Calculate jitter as a GstClockTimeDiff
                GstClockTimeDiff jitter = currentTime - expectedTime;
                gdouble jitterSeconds = static_cast<gdouble>(jitter) / GST_SECOND;

                // Use ostringstream for precise conversion and formatting
                std::ostringstream jitterStream;
                jitterStream << std::fixed << std::setprecision(6) << jitterSeconds;

                // Initialize averageProportion to 1.0 by default
                gdouble averageProportion = 1.0;

                // Calculate the proportion based on the difference in time and PTS if it's not the first frame
                if (previousPTS_ != 0 && previousTime_ != 0) {
                    gdouble proportion = static_cast<gdouble>(currentTime - previousTime_) /
                        static_cast<gdouble>(pts - previousPTS_);

                    // Update running average of proportion
                    cumulativeProportion_ += proportion;
                    proportionSampleCount_++;

                    // Calculate the average proportion
                    averageProportion = cumulativeProportion_ / static_cast<gdouble>(proportionSampleCount_);
                }

                LOG_DEBUG("VideoComponent", "Buffer drawn for: " + videoFile_);
                LOG_DEBUG("VideoComponent", "Buffer PTS: " + std::to_string(GST_TIME_AS_MSECONDS(pts)) + " ms");
                LOG_DEBUG("VideoComponent", "Current Time: " + std::to_string(GST_TIME_AS_MSECONDS(currentTime)) + " ms");
                LOG_DEBUG("VideoComponent", "Expected Time: " + std::to_string(GST_TIME_AS_MSECONDS(expectedTime)) + " ms");
                LOG_DEBUG("VideoComponent", "Jitter: " + jitterStream.str() + " seconds");
                LOG_DEBUG("VideoComponent", "Average Proportion: " + std::to_string(averageProportion));

                // Determine the QOS type based on the jitter value
                GstQOSType qosType = (jitter < 0) ? GST_QOS_TYPE_UNDERFLOW : GST_QOS_TYPE_OVERFLOW;

                // Create a QOS event using the calculated average proportion
                GstEvent* qosEvent = gst_event_new_qos(
                    qosType,                // The type of QOS (UNDERFLOW or OVERFLOW)
                    averageProportion,      // Proportion of real-time performance based on average or default 1.0
                    jitter,                 // The time difference of the last clock sync (GstClockTimeDiff)
                    pts                     // The timestamp of the buffer
                );

                if (qosEvent) {
                    GstPad* sinkPad = gst_element_get_static_pad(videoInst_->getVideoSink(), "sink");
                    if (sinkPad) {
                        gst_pad_push_event(sinkPad, qosEvent);
                        gst_object_unref(sinkPad);
                    }
                }
                else {
                    LOG_DEBUG("VideoComponent", "Failed to create QOS event.");
                }

                // Update the previous values for the next calculation
                previousTime_ = currentTime;
                previousPTS_ = pts;

                // Reset the new frame flag
                videoInst_->resetNewFrameFlag();
            }
        }
    }
}

bool VideoComponent::isPlaying()
{
    return videoInst_->isPlaying();
}

std::string_view VideoComponent::filePath()
{
    return videoFile_;
}

void VideoComponent::skipForward()
{
    if (videoInst_)
        videoInst_->skipForward();
}

void VideoComponent::skipBackward()
{
    if (videoInst_)
        videoInst_->skipBackward();
}

void VideoComponent::skipForwardp()
{
    if (videoInst_)
        videoInst_->skipForwardp();
}

void VideoComponent::skipBackwardp()
{
    if (videoInst_)
        videoInst_->skipBackwardp();
}

void VideoComponent::pause()
{
    if (videoInst_)
        videoInst_->pause();
}

void VideoComponent::restart()
{
    if (videoInst_)
        videoInst_->restart();
}

unsigned long long VideoComponent::getCurrent()
{
    if (videoInst_)
        return videoInst_->getCurrent();
    else
        return 0;
}

unsigned long long VideoComponent::getDuration()
{
    if (videoInst_)
        return videoInst_->getDuration();
    else
        return 0;
}

bool VideoComponent::isPaused()
{
    if (videoInst_)
        return videoInst_->isPaused();
    else
        return false;
}
