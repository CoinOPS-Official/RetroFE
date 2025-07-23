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
#include <wordexp.h> // For robust shell-like word expansion

#include "../../../Utility/Log.h"

 // --- Helper for wordexp RAII ---
struct WordExpWrapper {
    wordexp_t p;
    WordExpWrapper() { p.we_wordc = 0; }
    ~WordExpWrapper() { if (p.we_wordc > 0) wordfree(&p); }
};

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
    pid_t pid = fork();
    if (pid == 0) { // Child process
        // Detach from the parent's session completely.
        if (setsid() == -1) {
            perror("simpleLaunch: setsid failed");
            _exit(EXIT_FAILURE);
        }

        if (!currentDirectory.empty()) {
            if (chdir(currentDirectory.c_str()) != 0) {
                perror("simpleLaunch: chdir failed");
                _exit(EXIT_FAILURE);
            }
        }

        std::string commandLine = executable + " " + args;
        WordExpWrapper we;
        if (wordexp(commandLine.c_str(), &we.p, WRDE_NOCMD) != 0) {
            _exit(EXIT_FAILURE);
        }
        execvp(we.p.we_wordv[0], we.p.we_wordv);
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
    std::string commandLine = executable + " " + args;
    WordExpWrapper we;
    // WRDE_NOCMD prevents command substitution for security.
    if (wordexp(commandLine.c_str(), &we.p, WRDE_NOCMD) != 0) {
        LOG_ERROR("ProcessManager", "Failed to parse command line: " + commandLine);
        return false;
    }

    pid_ = fork();

    if (pid_ == 0) {
        // === CHILD PROCESS ===

        // *** The Process Group Trick for Robust Termination ***
        // Create a new session and process group, making this process the leader.
        // Now, signals sent to -pid_ will go to the entire group of descendants.
        if (setsid() == -1) {
            perror("launch: setsid failed");
            _exit(EXIT_FAILURE);
        }

        if (!currentDirectory.empty()) {
            if (chdir(currentDirectory.c_str()) != 0) {
                perror("launch: chdir failed");
                _exit(EXIT_FAILURE);
            }
        }

        // execvp requires a null-terminated array of char*
        execvp(we.p.we_wordv[0], we.p.we_wordv);

        // If execvp returns, it's an error.
        perror("launch: execvp failed");
        _exit(EXIT_FAILURE);
    }
    else if (pid_ > 0) {
        // === PARENT PROCESS ===
        LOG_INFO("ProcessManager", "Successfully forked process with group PID: " + std::to_string(pid_));
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
        LOG_INFO("ProcessManager", "Terminating process group " + std::to_string(pid_) + " with SIGKILL.");

        // *** The Process Group Trick in Action ***
        // By sending the signal to the negative PID, we signal the entire process group
        // that was created with setsid(). This is the Unix equivalent of TerminateJobObject.
        kill(-pid_, SIGKILL);

        waitpid(pid_, nullptr, 0);
        pid_ = -1;
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