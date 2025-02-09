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
#include "../Graphics/Page.h"

#include <cstdlib>

AttractMode::AttractMode()
    : idleTime(0)
    , idleNextTime(0)
    , idlePlaylistTime(0)
    , idleCollectionTime(0)
    , minScrollBeforeLaunchTime_(0)
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
{
}

void AttractMode::reset(bool set)
{
    elapsedTime_ = 0;
    isActive_ = false;
    isSet_ = set;
    activeTime_ = 0;
    if (!set) {
        elapsedPlaylistTime_ = 0;
        elapsedCollectionTime_ = 0;
    }
}

int AttractMode::update(float dt, Page& page)
{
    static float cooldownTime = 2.0f;  // Cooldown period in seconds
    static float cooldownElapsedTime = 0.0f;
    static float timeSinceLastLaunch = 0.0f;  // Time since last game launch

    // Timers for the main loop
    elapsedTime_ += dt;
    elapsedPlaylistTime_ += dt;
    elapsedCollectionTime_ += dt;
    timeSinceLastLaunch += dt;  // Increment time since the last launch

    // If shouldLaunch is true, use cooldown logic for game launch
    if (shouldLaunch && timeSinceLastLaunch >= minScrollBeforeLaunchTime_)
    {
        if (page.isJukebox())
        {
            if (!isActive_ && !page.isJukeboxPlaying() && elapsedTime_ > 10)
            {
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
            }
        }
        else
        {
            // Check if it's time to switch playlists
            if (!isActive_ && elapsedPlaylistTime_ > idlePlaylistTime && idlePlaylistTime > 0)
            {
                elapsedTime_ = 0;
                elapsedPlaylistTime_ = 0;
                cooldownElapsedTime = 0;  // Reset cooldown
                return 1;
            }

            // Check if it's time to switch collections
            if (!isActive_ && elapsedCollectionTime_ > idleCollectionTime && idleCollectionTime > 0)
            {
                elapsedTime_ = 0;
                elapsedPlaylistTime_ = 0;
                elapsedCollectionTime_ = 0;
                cooldownElapsedTime = 0;  // Reset cooldown
                return 2;
            }

            // Enable attract mode when idling for the expected time
            if (!isActive_ && ((elapsedTime_ > idleTime && idleTime > 0) || (isSet_ && elapsedTime_ > idleNextTime && idleNextTime > 0)))
            {
                if (!isSet_)
                    elapsedPlaylistTime_ = 0;  // Reset playlist timer if entering attract mode
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
                cooldownElapsedTime = 0;  // Reset cooldown
            }
        }

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
                        page.updateScrollPeriod();  // Accelerate scrolling
                    }
                }
                cooldownElapsedTime = 0;  // Reset cooldown if still within active time
            }
            else
            {
                // After scrolling stops, start the cooldown
                cooldownElapsedTime += dt;

                // Check if it's time to launch a random game
                if (cooldownElapsedTime >= cooldownTime)
                {
                    // Launch the game after the cooldown
                    elapsedTime_ = 0;
                    isActive_ = false;
                    timeSinceLastLaunch = 0.0f;  // Reset time since the last launch
                    return 3;  // Signal to RetroFe to launch a random game
                }
            }
        }
    }
    else  // Use original attract mode behavior when shouldLaunch is false
    {
        if (page.isJukebox()) {
            if (!isActive_ && !page.isJukeboxPlaying() && elapsedTime_ > 10) {
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
            }
        }
        else {
            // Check if it's time to switch playlists
            if (!isActive_ && elapsedPlaylistTime_ > idlePlaylistTime && idlePlaylistTime > 0) {
                elapsedTime_ = 0;
                elapsedPlaylistTime_ = 0;
                return 1;
            }

            // Check if it's time to switch collections
            if (!isActive_ && elapsedCollectionTime_ > idleCollectionTime && idleCollectionTime > 0)
            {
                elapsedTime_ = 0;
                elapsedPlaylistTime_ = 0;
                elapsedCollectionTime_ = 0;
                return 2;
            }

            // Enable attract mode when idling for the expected time
            if (!isActive_ && ((elapsedTime_ > idleTime && idleTime > 0) || (isSet_ && elapsedTime_ > idleNextTime && idleNextTime > 0)))
            {
                if (!isSet_)
                    elapsedPlaylistTime_ = 0;  // Reset playlist timer if entering attract mode
                isActive_ = true;
                isSet_ = true;
                elapsedTime_ = 0;
                activeTime_ = ((float)(minTime + (rand() % (maxTime - minTime)))) / 1000;
            }
        }

        if (isActive_) {
            if (page.isMenuIdle()) {
                page.setScrolling(Page::ScrollDirectionForward);
                page.scroll(true, false);
                if (isFast) {
                    page.updateScrollPeriod(); // accelerate scrolling
                }
            }

            if (elapsedTime_ > activeTime_) {
                elapsedTime_ = 0;
                isActive_ = false;
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