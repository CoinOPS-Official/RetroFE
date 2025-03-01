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
#include "AttractMode.h"
#include "../Utility/Log.h"
#include "../Graphics/Page.h"
#include <cstdlib>
#include <ctime>
#include <string>

AttractMode::AttractMode()
    : idleTime(0)
    , idleNextTime(0)
    , idlePlaylistTime(0)
    , idleCollectionTime(0)
    , minTime(0)
    , maxTime(0)
    , isFast(false)
    , shouldLaunch(false)
    , isActive_(false)
    , isSet_(false)
    , elapsedTime_(0)
    , elapsedPlaylistTime_(0)
    , elapsedCollectionTime_(0)
    , activeTime_(0)
    , cooldownTime_(2.0f)           // Make configurable
    , cooldownElapsedTime_(0)
    , launchInitiated_(false)
    , currentState_(State::IDLE)
    , stateTransitionTime_(0)
    , minStateTime_(5.0f)
    , idleCycleCount_(0)
    , minLaunchCycles_(3)      // Default: launch after at least 3 cycles
    , maxLaunchCycles_(5)      // Default: launch after at most 5 cycles
    , targetLaunchCycles_(0)   // Will be set in updateLaunchTarget
{
    // Initialize random number generator 
    srand((unsigned int)time(NULL));

    // Set initial target
    updateLaunchTarget();
}

void AttractMode::reset(bool set) {
    elapsedTime_ = 0;
    isActive_ = false;
    isSet_ = set;
    activeTime_ = 0;
    launchInitiated_ = false;
    cooldownElapsedTime_ = 0;

    // DON'T reset idleCycleCount_ here - it needs to accumulate
    // idleCycleCount_ = 0;  // REMOVE THIS LINE

    // Reset state
    setState(State::IDLE, 0);

    // Only update launch target on a full reset
    if (!set) {
        updateLaunchTarget();
        idleCycleCount_ = 0;  // Only reset counter on full reset
        elapsedPlaylistTime_ = 0;
        elapsedCollectionTime_ = 0;
    }
}

int AttractMode::update(float dt, Page& page) {
    // Track total time for state management
    float currentTime = elapsedTime_ + dt;

    // Update main timers
    elapsedTime_ = currentTime;
    elapsedPlaylistTime_ += dt;
    elapsedCollectionTime_ += dt;

    float timeInCurrentState = currentTime - stateTransitionTime_;

    // Static variable to track previous canLaunch state to reduce log spam
    static bool prevCanLaunch = false;

    // FIRST - Handle state transitions for playlist/collection changes

    // Check for playlist changes 
    if (!isActive_ && elapsedPlaylistTime_ > idlePlaylistTime && idlePlaylistTime > 0)
    {
        // Reset timers when changing playlists
        elapsedTime_ = 0;
        elapsedPlaylistTime_ = 0;
        setState(State::PLAYLIST_CHANGED, 0);
        return 1;  // Signal playlist change
    }

    // Check for collection changes
    if (!isActive_ && elapsedCollectionTime_ > idleCollectionTime && idleCollectionTime > 0)
    {
        // Reset timers when changing collections
        elapsedTime_ = 0;
        elapsedPlaylistTime_ = 0;
        elapsedCollectionTime_ = 0;
        setState(State::COLLECTION_CHANGED, 0);
        return 2;  // Signal collection change
    }

    // If in a transition state and enough time has passed, move to IDLE
    if ((currentState_ == State::PLAYLIST_CHANGED || currentState_ == State::COLLECTION_CHANGED) &&
        timeInCurrentState >= minStateTime_) {
        setState(State::IDLE, currentTime);
    }

    // SECOND - Determine if we can launch, only logging on state change

    // Prevent launching if we recently changed playlist or collection
    bool canLaunch = shouldLaunch &&
        (currentState_ != State::PLAYLIST_CHANGED) &&
        (currentState_ != State::COLLECTION_CHANGED);

    // Only log when canLaunch changes to reduce log spam
    if (canLaunch != prevCanLaunch) {
        LOG_DEBUG("AttractMode", "Can Launch changed: " + std::string(canLaunch ? "YES" : "NO") +
            " | shouldLaunch: " + std::string(shouldLaunch ? "YES" : "NO"));
        prevCanLaunch = canLaunch;
    }

    // THIRD - Handle the active state machine

    // Handle cooldown and launch states first - these have priority
    if (currentState_ == State::COOLDOWN) {
        cooldownElapsedTime_ += dt;

        // Log cooldown progress occasionally (every half second)
        if (fmod(cooldownElapsedTime_, 0.5f) < dt) {
            LOG_INFO("AttractMode", "Launch cooldown: " + std::to_string(cooldownElapsedTime_) + "/" +
                std::to_string(cooldownTime_) + "s");
        }

        // Check if it's time to launch
        if (cooldownElapsedTime_ >= cooldownTime_)
        {
            LOG_INFO("AttractMode", "Launch sequence initiated");
            setState(State::LAUNCH_READY, currentTime);
            elapsedTime_ = 0;
            isActive_ = false;
            cooldownElapsedTime_ = 0.0f;
            return 3;  // Signal to launch a game
        }

        // Skip the rest of the update when in cooldown
        return 0;
    }

    // If we can launch, handle attract mode entry and cycles
    if (canLaunch)
    {
        if (page.isJukebox())
        {
            // Jukebox-specific logic
            if (!isActive_ && !page.isJukeboxPlaying() && elapsedTime_ > 10)
            {
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
                setState(State::SCROLLING, currentTime);
                LOG_DEBUG("AttractMode", "Starting jukebox scroll phase, duration: " +
                    std::to_string(activeTime_) + "s");
            }
        }
        else
        {
            // Enable attract mode when idling for the expected time
            if (!isActive_ && ((elapsedTime_ > idleTime && idleTime > 0) ||
                (isSet_ && elapsedTime_ > idleNextTime && idleNextTime > 0)))
            {
                if (!isSet_)
                    elapsedPlaylistTime_ = 0;
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
                setState(State::SCROLLING, currentTime);
                LOG_DEBUG("AttractMode", "Starting scroll phase, duration: " +
                    std::to_string(activeTime_) + "s");
            }
        }

        // Handle active attract mode scrolling
        if (isActive_)
        {
            // Scroll if we're within active time
            if (elapsedTime_ < activeTime_)
            {
                if (page.isMenuIdle())
                {
                    page.setScrolling(Page::ScrollDirectionForward);
                    page.scroll(true, false);
                    if (isFast)
                    {
                        page.updateScrollPeriod();
                    }
                }

                // Make sure we're in SCROLLING state
                if (currentState_ != State::SCROLLING) {
                    setState(State::SCROLLING, currentTime);
                }
            }
            else  // Scrolling phase completed
            {
                // IMPORTANT: Only increment cycle counter when transitioning FROM SCROLLING
                if (currentState_ == State::SCROLLING) {
                    idleCycleCount_++;
                    LOG_INFO("AttractMode", "Idle cycle completed: " + std::to_string(idleCycleCount_) +
                        "/" + std::to_string(targetLaunchCycles_) + " cycles");

                    // Check if we've reached target cycles
                    if (idleCycleCount_ >= targetLaunchCycles_) {
                        LOG_INFO("AttractMode", "Target of " + std::to_string(targetLaunchCycles_) +
                            " cycles reached, preparing for launch");
                        setState(State::COOLDOWN, currentTime);
                        cooldownElapsedTime_ = 0.0f;
                        idleCycleCount_ = 0;  // Reset counter
                        updateLaunchTarget();
                    }
                    else {
                        // Not enough cycles, go back to IDLE
                        setState(State::IDLE, currentTime);
                        isActive_ = false;
                        elapsedTime_ = 0;
                    }
                }
            }
        }
    }
    else  // Original attract mode behavior without launching
    {
        // Normal attract mode without launching capability
        if (page.isJukebox()) {
            if (!isActive_ && !page.isJukeboxPlaying() && elapsedTime_ > 10) {
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
                setState(State::SCROLLING, currentTime);
            }
        }
        else {
            // Enable attract mode when idling for the expected time
            if (!isActive_ && ((elapsedTime_ > idleTime && idleTime > 0) ||
                (isSet_ && elapsedTime_ > idleNextTime && idleNextTime > 0)))
            {
                if (!isSet_)
                    elapsedPlaylistTime_ = 0;
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
                setState(State::SCROLLING, currentTime);
            }
        }

        if (isActive_) {
            if (page.isMenuIdle()) {
                page.setScrolling(Page::ScrollDirectionForward);
                page.scroll(true, false);
                if (isFast) {
                    page.updateScrollPeriod();
                }
            }

            if (elapsedTime_ > activeTime_) {
                elapsedTime_ = 0;
                isActive_ = false;
                setState(State::IDLE, currentTime);
                // NOTE: We DO NOT increment idleCycleCount_ in the non-launch path
            }
        }
    }

    return 0;  // Continue attract mode
}

bool AttractMode::isActive() const
{
    return isActive_;
}

void AttractMode::activate()
{
    isActive_ = true;
}


bool AttractMode::isSet() const
{
    return isSet_;
}

void AttractMode::setState(State newState, float currentTime) {
    // Only log transitions to different states
    if (newState != currentState_) {
        // Debug log state transitions
        std::string fromState = stateToString(currentState_);
        std::string toState = stateToString(newState);
        LOG_DEBUG("AttractMode", "State change: " + fromState + " -> " + toState);

        currentState_ = newState;
        stateTransitionTime_ = currentTime;

        // Reset appropriate timers based on new state
        if (newState == State::PLAYLIST_CHANGED || newState == State::COLLECTION_CHANGED) {
            cooldownElapsedTime_ = 0;
            launchInitiated_ = false;
        }
        else if (newState == State::IDLE) {
            isActive_ = false;
        }
    }
}

// Helper function for state names in logs
std::string AttractMode::stateToString(State state) {
    switch (state) {
        case State::IDLE: return "IDLE";
        case State::SCROLLING: return "SCROLLING";
        case State::COOLDOWN: return "COOLDOWN";
        case State::PLAYLIST_CHANGED: return "PLAYLIST_CHANGED";
        case State::COLLECTION_CHANGED: return "COLLECTION_CHANGED";
        case State::LAUNCH_READY: return "LAUNCH_READY";
        default: return "UNKNOWN";
    }
}

void AttractMode::setLaunchFrequencyRange(int minCycles, int maxCycles) {
    minLaunchCycles_ = minCycles;
    maxLaunchCycles_ = maxCycles;

    // Make sure min <= max
    if (minLaunchCycles_ > maxLaunchCycles_)
        minLaunchCycles_ = maxLaunchCycles_;

    // Update target with new range
    updateLaunchTarget();
}

void AttractMode::updateLaunchTarget() {
    // Generate random value between min and max (inclusive)
    int range = maxLaunchCycles_ - minLaunchCycles_ + 1;
    targetLaunchCycles_ = minLaunchCycles_ + (rand() % range);

    // Optional debug logging
    LOG_DEBUG("AttractMode", "New launch target set: " + std::to_string(targetLaunchCycles_) + " cycles");
}