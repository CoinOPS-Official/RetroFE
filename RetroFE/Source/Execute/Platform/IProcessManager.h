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
#include <functional>

// Represents the outcome of a wait/monitoring operation
enum class WaitResult { ProcessExit, UserInput, Timeout, Error };

// Callback for the UI to render a frame during a wait loop
using FrameTickCallback = std::function<void()>;

class IProcessManager {
public:
    virtual ~IProcessManager() = default;

    // A simple launch-and-forget execution
    virtual bool simpleLaunch(const std::string& executable, const std::string& args, const std::string& currentDirectory) = 0;

    // A complex launch for full monitoring
    virtual bool launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) = 0;

    // Waits for the process with callbacks
    virtual WaitResult wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) = 0;

    // Forcibly terminates the process
    virtual void terminate() = 0;

    // Best-effort exit code (true if known and process has exited)
    virtual bool tryGetExitCode(int& outExitCode) const = 0;
};