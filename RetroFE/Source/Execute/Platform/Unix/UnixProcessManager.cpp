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
#include "UnixProcessManager.h"

#include <unistd.h>     // For fork, execvp, chdir, getpid
#include <sys/wait.h>   // For waitpid
#include <signal.h>     // For kill
#include <sstream>
#include <iomanip>      // For std::quoted
#include <thread>       // For std::this_thread::sleep_for
#include <vector>

#include "../../../Utility/Log.h"

UnixProcessManager::UnixProcessManager() {
    LOG_INFO("ProcessManager", "UnixProcessManager created.");
}

UnixProcessManager::~UnixProcessManager() {
    // Ensure any lingering child process is dealt with, though it should be handled by terminate() or wait().
    if (isRunning()) {
        LOG_WARNING("ProcessManager", "UnixProcessManager destroyed while process " + std::to_string(pid_) + " was still running. Terminating.");
        terminate();
    }
}

// --- Public Interface Implementation ---

bool UnixProcessManager::simpleLaunch(const std::string& executable, const std::string& args, const std::string& currentDirectory) {
    // Use the main launch function but immediately detach from the child.
    // This is done by not storing the PID and letting the child run independently.
    // The OS will re-parent it to `init` when we exit.
    pid_t pid = fork();
    if (pid == 0) { // Child process
        // Same logic as launch()
        if (!currentDirectory.empty()) {
            if (chdir(currentDirectory.c_str()) != 0) {
                perror("simpleLaunch: chdir failed");
                _exit(EXIT_FAILURE);
            }
        }
        std::vector<std::string> argVector;
        std::istringstream argsStream(args);
        std::string arg;
        while (argsStream >> std::quoted(arg)) {
            argVector.push_back(arg);
        }
        std::vector<char*> execArgs;
        execArgs.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : argVector) {
            execArgs.push_back(const_cast<char*>(a.c_str()));
        }
        execArgs.push_back(nullptr);
        execvp(executable.c_str(), execArgs.data());
        perror("simpleLaunch: execvp failed");
        _exit(EXIT_FAILURE);
    }
    else if (pid < 0) { // Fork failed
        LOG_ERROR("ProcessManager", "simpleLaunch fork failed.");
        return false;
    }
    // Parent process: success, do nothing and return.
    return true;
}

bool UnixProcessManager::launch(const std::string& executable, const std::string& args, const std::string& currentDirectory) {
    pid_ = fork();

    if (pid_ == 0) {
        // === CHILD PROCESS ===
        // Change working directory if specified
        if (!currentDirectory.empty()) {
            if (chdir(currentDirectory.c_str()) != 0) {
                // Cannot use LOG_* here as it's not async-signal-safe.
                // Write directly to stderr.
                perror("launch: chdir failed");
                _exit(EXIT_FAILURE);
            }
        }

        // Parse arguments string into a vector
        std::vector<std::string> argVector;
        std::istringstream argsStream(args);
        std::string arg;
        while (argsStream >> std::quoted(arg)) {
            argVector.push_back(arg);
        }

        // Build the C-style argv array for execvp
        std::vector<char*> execArgs;
        execArgs.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : argVector) {
            execArgs.push_back(const_cast<char*>(a.c_str()));
        }
        execArgs.push_back(nullptr);

        // Replace the child process with the target program
        execvp(executable.c_str(), execArgs.data());

        // If execvp returns, it's an error.
        perror("launch: execvp failed");
        _exit(EXIT_FAILURE); // Exit child immediately
    }
    else if (pid_ > 0) {
        // === PARENT PROCESS ===
        LOG_INFO("ProcessManager", "Successfully forked process with PID: " + std::to_string(pid_));
        return true;
    }
    else {
        // === FORK FAILED ===
        LOG_ERROR("ProcessManager", "Failed to fork a new process.");
        pid_ = -1;
        return false;
    }
}

WaitResult UnixProcessManager::wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) {
    if (!isRunning()) {
        LOG_ERROR("ProcessManager", "Wait called but no process is running.");
        return WaitResult::Error;
    }

    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        // --- 1. Let the frontend do its thing ---
        if (onFrameTick) {
            onFrameTick();
        }

        // --- 2. Check for user input ---
        if (userInputCheck && userInputCheck()) {
            return WaitResult::UserInput;
        }

        // --- 3. Check for process exit (non-blocking) ---
        int status;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            LOG_INFO("ProcessManager", "Process " + std::to_string(pid_) + " has exited.");
            pid_ = -1; // Mark as not running
            return WaitResult::ProcessExit;
        }

        // --- 4. Check for timeout ---
        if (timeoutSeconds > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (elapsedSec >= timeoutSeconds) {
                return WaitResult::Timeout;
            }
        }

        // --- 5. Yield to prevent busy-waiting ---
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
    }
}

void UnixProcessManager::terminate() {
    if (isRunning()) {
        LOG_INFO("ProcessManager", "Terminating process " + std::to_string(pid_) + " with SIGKILL.");
        kill(pid_, SIGKILL);
        // After killing, we must wait for it to be reaped by the system.
        waitpid(pid_, nullptr, 0);
        pid_ = -1; // Mark as not running
    }
    else {
        LOG_WARNING("ProcessManager", "Terminate called but no process was running.");
    }
}

bool UnixProcessManager::isRunning() const {
    if (pid_ <= 0) {
        return false;
    }
    // Use kill with signal 0 to check for process existence without sending a signal.
    return (kill(pid_, 0) == 0);
}
#endif