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
#pragma once

#include <string>

class Page;

class AttractMode
{
public:
    
    enum class State {
        IDLE,               // Not in attract mode
        SCROLLING,          // In attract mode, scrolling through items
        COOLDOWN,           // After scrolling, waiting for cooldown
        PLAYLIST_CHANGED,   // Just switched playlists
        COLLECTION_CHANGED, // Just switched collections
        LAUNCH_READY        // Ready to launch a game
    };
    
    AttractMode();
    void reset( bool set = false );
    int   update(float dt, Page &page);
    float idleTime;
    float idleNextTime;
    float idlePlaylistTime;
    float idleCollectionTime;
    float launchDelayTimer_;
	int   minTime;
	int   maxTime;
    bool isFast;
    bool shouldLaunch;
    bool  isActive() const;
    void activate();
    bool  isSet() const;
    State getState() const { return currentState_; }
    void setLaunchFrequencyRange(int minCycles, int maxCycles);

private:
    bool isActive_;
    bool isSet_;
    float elapsedTime_;
    float elapsedPlaylistTime_;
    float elapsedCollectionTime_;
    float activeTime_;
    float cooldownTime_;            // Instead of static
    float cooldownElapsedTime_;     // Instead of static
    bool cooldownAfterSwitch_;
    bool launchInitiated_;          // New: Track if launch has been initiated
    State currentState_;             // Current state of attract mode
    float stateTransitionTime_;      // Time when last state change occurred
    float minStateTime_;             // Minimum time to stay in a state
    void setState(State newState, float currentTime);
    std::string stateToString(State state);
    void setMinStateTime(float time) { minStateTime_ = time; }

    // New private members for launch frequency
    int idleCycleCount_;      // Current count of completed attract mode cycles
    int minLaunchCycles_;     // Minimum cycles before launching a game
    int maxLaunchCycles_;     // Maximum cycles before launching a game
    int targetLaunchCycles_;  // Current target (random between min and max)

    // New helper method to update launch target
    void updateLaunchTarget();

};