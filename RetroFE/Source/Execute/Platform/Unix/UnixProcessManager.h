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

#ifndef WIN32
#pragma once

#include <sys/types.h> // For pid_t

#include "../IProcessManager.h" // Include the interface we are implementing

/**
 * @brief A Unix/POSIX-specific implementation of the IProcessManager interface.
 *
 * This class handles process management using the standard fork/exec/waitpid model.
 */
class UnixProcessManager : public IProcessManager {
public:
    /**
     * @brief Constructs the UnixProcessManager.
     */
    UnixProcessManager();

    /**
     * @brief Destructor.
     */
    ~UnixProcessManager() override;

    // --- Public Interface Implementation ---

    bool simpleLaunch(const std::string& executable, const std::string& args, const std::string& currentDirectory) override;
    bool launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) override;
    WaitResult wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) override;
    void terminate() override;

    // This class manages a system resource and should not be copied.
    UnixProcessManager(const UnixProcessManager&) = delete;
    UnixProcessManager& operator=(const UnixProcessManager&) = delete;

private:
    pid_t pid_ = -1; // Process ID of the forked child process.
    pid_t pgid_ = -1;
    bool isRunning() const;

};
#endif