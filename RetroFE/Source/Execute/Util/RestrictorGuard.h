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

#include "../../Control/Restrictor/RestrictorInstance.h"
#include "../../Utility/Log.h"

/**
 * @brief An RAII scope guard to manage the state of a hardware restrictor (e.g., ServoStik).
 *
 * This class sets the restrictor to a specified mode upon construction and
 * automatically restores it to the default 8-way mode upon destruction.
 * This ensures the restrictor state is always cleaned up, even if errors occur.
 */
class RestrictorGuard {
public:
    /**
     * @brief Constructs the guard and sets the restrictor to the desired mode.
     * @param way The mode to set the restrictor to (e.g., 4 for 4-way).
     */
    explicit RestrictorGuard(int way) {
        // gRestrictor is assumed to be a globally accessible pointer/instance
        if (gRestrictor && gRestrictor->setWay(way)) {
                LOG_INFO("RestrictorGuard", "Restrictor set to " + std::to_string(way) + "-way mode.");
                wasSet_ = true;
            }
            else {
                LOG_ERROR("RestrictorGuard", "Failed to set restrictor to " + std::to_string(way) + "-way mode.");
            }
        }

    /**
     * @brief Destructs the guard and restores the restrictor to its default 8-way mode.
     */
    ~RestrictorGuard() {
        if (wasSet_) {
            if (gRestrictor && gRestrictor->setWay(8)) {
                LOG_INFO("RestrictorGuard", "Returned restrictor to 8-way mode.");
            }
            else {
                LOG_ERROR("RestrictorGuard", "Failed to return restrictor to 8-way mode.");
            }
        }
    }

    // Disable copying and assignment to prevent incorrect resource management.
    RestrictorGuard(const RestrictorGuard&) = delete;
    RestrictorGuard& operator=(const RestrictorGuard&) = delete;

private:
    bool wasSet_ = false; // Tracks if the initial set was successful.
};