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
#include <fcntl.h> // For open, O_WRONLY

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
    LOG_INFO("ProcessManager", "Attempting to launch: " + executable);
    if (!args.empty()) {
        LOG_INFO("ProcessManager", "           Arguments: " + args);
    }
    if (!currentDirectory.empty()) {
        LOG_INFO("ProcessManager", "     Working directory: " + currentDirectory);
    }
    
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

        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        // execvp requires a null-terminated array of char*
        execvp(we.p.we_wordv[0], we.p.we_wordv);

        // If execvp returns, it's an error.
        perror("launch: execvp failed");
        _exit(EXIT_FAILURE);
    }
    else if (pid_ > 0) {
        // === PARENT PROCESS ===
        pgid_ = pid_;  // Save the process group ID for this launched process
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
    // This timer will control our ~30 FPS rendering and logic tick.
    auto lastFrameTime = startTime;

    while (true) {
        // --- 1. HIGH-FREQUENCY INPUT POLLING ---
        // We poll for input on every single spin of this tight `while` loop.
        // This ensures maximum responsiveness and prevents missed combos.
        if (userInputCheck && userInputCheck()) {
            return WaitResult::UserInput;
        }

        // --- 2. THROTTLED LOGIC AND RENDERING (~30 FPS) ---
        auto now = std::chrono::steady_clock::now();
        auto elapsedFrameMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime).count();

        // Only run the "heavy" logic if ~33ms have passed since the last frame.
        if (elapsedFrameMs >= 33) {
            // A. Call the expensive onFrameTick to render graphics.
            if (onFrameTick) {
                onFrameTick();
            }

            // B. Check for process exit. This doesn't need to be checked 1000x per second.
            int status;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                LOG_INFO("ProcessManager", "Process " + std::to_string(pid_) + " has exited.");
                pid_ = -1; // Mark as not running
                return WaitResult::ProcessExit;
            }

            // C. Check for the main timeout.
            if (timeoutSeconds > 0) {
                auto elapsedTotalSec = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
                if (elapsedTotalSec >= timeoutSeconds) {
                    return WaitResult::Timeout;
                }
            }

            // Reset the timer for the next frame.
            lastFrameTime = now;
        }

        // --- 3. YIELD TO THE OS ---
        // A very short sleep to prevent this tight loop from consuming 100% CPU,
        // while still allowing it to spin very fast for input polling.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void UnixProcessManager::terminate() {
    if (!isRunning()) {
        LOG_WARNING("ProcessManager", "Terminate called but no process was running.");
        return;
    }

    // Prefer the recorded process group (child does setsid()). Fall back to PID if needed.
    const pid_t target_pgid = (pgid_ > 0 ? pgid_ : pid_);
    const pid_t target_pid = pid_;

    LOG_INFO("ProcessManager",
        "Attempting graceful termination of process group "
        + std::to_string(target_pgid) + " (child pid " + std::to_string(target_pid) + ") with SIGTERM.");

    // --- helpers (inline, so we keep everything in this function and retain logging) ---
    auto send_group = [&](int sig, const char* name) {
        int rc = kill(-target_pgid, sig);
        if (rc == -1) {
            LOG_ERROR("ProcessManager", std::string("Failed to send ") + name +
                " to PGID " + std::to_string(target_pgid) +
                " (errno=" + std::to_string(errno) + ")."
                " Falling back to PID " + std::to_string(target_pid) + ".");
            if (kill(target_pid, sig) == -1) {
                LOG_ERROR("ProcessManager", std::string("Also failed to send ") + name +
                    " to PID " + std::to_string(target_pid) +
                    " (errno=" + std::to_string(errno) + ").");
            }
        }
        else {
            LOG_INFO("ProcessManager", std::string("Sent ") + name + " to PGID " + std::to_string(target_pgid) + ".");
        }
        };

    auto waitChildExitTimed = [&](int ms_timeout) -> bool {
        int status = 0;
        const int step_ms = 50;
        for (int waited = 0; waited <= ms_timeout; waited += step_ms) {
            pid_t r = waitpid(target_pid, &status, WNOHANG);
            if (r == target_pid) {
                if (WIFEXITED(status)) {
                    LOG_INFO("ProcessManager", "Child " + std::to_string(target_pid) +
                        " exited with code " + std::to_string(WEXITSTATUS(status)) + ".");
                }
                else if (WIFSIGNALED(status)) {
                    LOG_INFO("ProcessManager", "Child " + std::to_string(target_pid) +
                        " killed by signal " + std::to_string(WTERMSIG(status)) + ".");
                }
                else {
                    LOG_INFO("ProcessManager", "Child " + std::to_string(target_pid) + " reaped.");
                }
                return true;
            }
            if (r < 0) {
                if (errno == ECHILD) {
                    LOG_INFO("ProcessManager", "No child to reap (ECHILD). Treating as already exited.");
                    return true;
                }
                LOG_WARNING("ProcessManager", "waitpid transient error (errno=" + std::to_string(errno) + ").");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        }
        return false; // still not gone
        };

    auto reapAllChildrenNonBlocking = [&]() {
        int total = 0;
        for (;;) {
            int status = 0;
            pid_t r = waitpid(-1, &status, WNOHANG);
            if (r > 0) {
                ++total;
                continue;
            }
            if (r == 0 || (r < 0 && errno == ECHILD)) break;
            // other error: bail
            break;
        }
        if (total > 0) {
            LOG_INFO("ProcessManager", "Reaped " + std::to_string(total) + " additional child(ren) (zombies/helpers).");
        }
        };
    // --- end helpers ---

    // 1) Polite: SIGTERM to the group, short bounded wait
    send_group(SIGTERM, "SIGTERM");
    if (waitChildExitTimed(500)) {
        reapAllChildrenNonBlocking();
        pid_ = -1;
        pgid_ = -1;
        LOG_INFO("ProcessManager", "Process group terminated gracefully after SIGTERM.");
        return;
    }

    // 2) Sledgehammer: SIGKILL to the group, short bounded wait
    LOG_WARNING("ProcessManager", "Process group did not respond to SIGTERM. Escalating to SIGKILL.");
    send_group(SIGKILL, "SIGKILL");
    (void)waitChildExitTimed(2000); // best effort
    reapAllChildrenNonBlocking();

    // 3) Cleanup state regardless; if anything remains, it’s not our child anymore
    pid_ = -1;
    pgid_ = -1;
    LOG_INFO("ProcessManager", "Terminate() complete.");
}


bool UnixProcessManager::isRunning() const {
    if (pid_ <= 0) {
        return false;
    }
    // Use kill with signal 0 to check for process existence without sending a signal.
    return (kill(pid_, 0) == 0);
}
#endif