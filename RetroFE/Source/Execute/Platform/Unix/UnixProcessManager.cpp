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
#include <cstring> // For strerror
#include <cerrno>  // For errno

#include "../../../Utility/Log.h"

 // --- Helper for wordexp RAII ---
struct WordExpWrapper {
    wordexp_t p;
    WordExpWrapper() { p.we_wordc = 0; }
    ~WordExpWrapper() { if (p.we_wordc > 0) wordfree(&p); }
};

UnixProcessManager::UnixProcessManager() {
    LOG_INFO("ProcessManager", "UnixProcessManager created.");
    lastExitCode_ = -1;
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

bool UnixProcessManager::launch(const std::string& executable,
    const std::string& args,
    const std::string& currentDirectory) {
    LOG_INFO("ProcessManager", "Attempting to launch: " + executable);
    if (!args.empty()) {
        LOG_INFO("ProcessManager", "           Arguments: " + args);
    }
    if (!currentDirectory.empty()) {
        LOG_INFO("ProcessManager", "     Working directory: " + currentDirectory);
    }

    // Build argv via wordexp (blocks command substitution)
    std::string commandLine = executable + " " + args;
    WordExpWrapper we;
    if (wordexp(commandLine.c_str(), &we.p, WRDE_NOCMD) != 0) {
        LOG_ERROR("ProcessManager", "Failed to parse command line: " + commandLine);
        return false;
    }

    // Pipe for exec result: child writes errno on failure; on success CLOEXEC closes it.
    int fds[2];
#if defined(__linux__)
    if (pipe2(fds, O_CLOEXEC) != 0) {
        LOG_ERROR("ProcessManager", "pipe2(O_CLOEXEC) failed (errno=" + std::to_string(errno) + ")");
        return false;
    }
#else
    if (pipe(fds) != 0) {
        LOG_ERROR("ProcessManager", "pipe() failed (errno=" + std::to_string(errno) + ")");
        return false;
    }
    // Set CLOEXEC manually (best-effort)
    fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD) | FD_CLOEXEC);
#endif

    pid_ = fork();

    if (pid_ == 0) {
        // ---------------- CHILD ----------------
        // Child: we will write to fds[1] if anything fails before exec.
        // Close the read end in child.
        close(fds[0]);

        // Start a new session/process group so parent can signal -pgid_ safely.
        if (setsid() == -1) {
            int e = errno; (void)!write(fds[1], &e, sizeof(e)); _exit(127);
        }

        if (!currentDirectory.empty() && chdir(currentDirectory.c_str()) != 0) {
            int e = errno; (void)!write(fds[1], &e, sizeof(e)); _exit(127);
        }

        // Silence child stdout/stderr.
        if (int devnull = open("/dev/null", O_WRONLY); devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        // Exec. On success, CLOEXEC will close fds[1] so parent sees EOF.
        execvp(we.p.we_wordv[0], we.p.we_wordv);

        // If we got here, exec failed. Send errno to parent, then exit.
        {
            int e = errno;
            (void)!write(fds[1], &e, sizeof(e));
        }
        _exit(127);
    }

    // ---------------- PARENT ----------------
    if (pid_ < 0) {
        // fork failed
        close(fds[0]); close(fds[1]);
        LOG_ERROR("ProcessManager", "Failed to fork a new process (errno=" + std::to_string(errno) + ")");
        pid_ = -1;
        pgid_ = -1;
        return false;
    }

    // Provisional pgid (valid if child succeeds)
    pgid_ = pid_;

    // Parent: close write end, read errno (if any)
    close(fds[1]);

    // Blocking read of exactly sizeof(int). EOF => success.
    int child_errno = 0;
    ssize_t nread = 0;
    while (true) {
        nread = read(fds[0], &child_errno, sizeof(child_errno));
        if (nread >= 0) break;
        if (errno == EINTR) continue;
        // Read error: treat as failure conservatively
        LOG_ERROR("ProcessManager", "Launch status read failed (errno=" + std::to_string(errno) + ")");
        (void)waitpid(pid_, nullptr, 0);
        close(fds[0]);
        pid_ = -1; pgid_ = -1;
        return false;
    }
    close(fds[0]);

    if (nread == 0) {
        // EOF: exec succeeded (CLOEXEC closed the fd in the child).
        LOG_INFO("ProcessManager", "Successfully forked & exec'd; group PID: " + std::to_string(pid_));
        return true;
    }

    if (nread == sizeof(child_errno)) {
        // Child reported exec failure. Reap and report.
        int status = 0; (void)waitpid(pid_, &status, 0);
        LOG_ERROR("ProcessManager",
            "execvp failed (" + std::to_string(child_errno) + "): " + std::string(strerror(child_errno)));
        pid_ = -1; pgid_ = -1;
        return false;
    }

    // Short/partial read: unexpected—treat as failure.
    {
        int status = 0; (void)waitpid(pid_, &status, 0);
        LOG_ERROR("ProcessManager", "Launch status unknown (short read). Treating as failure.");
        pid_ = -1; pgid_ = -1;
        return false;
    }
}

bool UnixProcessManager::tryGetExitCode(int& outExitCode) const {
    if (pid_ > 0) return false; // still running or not yet reaped
    if (lastExitCode_ < 0) return false; // unknown
    outExitCode = lastExitCode_;
    return true;
}

WaitResult UnixProcessManager::wait(double timeoutSeconds, const std::function<bool()>& userInputCheck, const FrameTickCallback& onFrameTick) {
    if (!isRunning()) {
        LOG_ERROR("ProcessManager", "Wait called but no process is running.");
        return WaitResult::Error;
    }

    auto startTime = std::chrono::steady_clock::now();
    auto lastFrameTime = startTime;

    while (true) {
        if (userInputCheck && userInputCheck()) {
            return WaitResult::UserInput;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsedFrameMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime).count();

        if (elapsedFrameMs >= 33) {
            if (onFrameTick) onFrameTick();

            int status = 0;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                // NEW: capture a meaningful exit code
                if (WIFEXITED(status))      lastExitCode_ = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) lastExitCode_ = 128 + WTERMSIG(status);
                else                          lastExitCode_ = 0; // generic “ended” code

                LOG_INFO("ProcessManager", "Process " + std::to_string(pid_) + " has exited with code " + std::to_string(lastExitCode_) + ".");
                pid_ = -1;
                return WaitResult::ProcessExit;
            }

            if (timeoutSeconds > 0) {
                auto elapsedTotalSec = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
                if (elapsedTotalSec >= timeoutSeconds) {
                    return WaitResult::Timeout;
                }
            }
            lastFrameTime = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void UnixProcessManager::terminate() {
    if (!isRunning()) {
        LOG_WARNING("ProcessManager", "Terminate called but no process was running.");
        return;
    }

    const pid_t target_pgid = (pgid_ > 0 ? pgid_ : pid_);
    const pid_t target_pid = pid_;

    LOG_INFO("ProcessManager", "Attempting graceful termination of process group " + std::to_string(target_pgid) + " (child pid " + std::to_string(target_pid) + ") with SIGTERM.");

    auto waitChildExitTimed = [&](int ms_timeout) -> bool {
        int status = 0;
        const int step_ms = 50;
        for (int waited = 0; waited <= ms_timeout; waited += step_ms) {
            pid_t r = waitpid(target_pid, &status, WNOHANG);
            if (r == target_pid) {
                if (WIFEXITED(status))      lastExitCode_ = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) lastExitCode_ = 128 + WTERMSIG(status);
                else                          lastExitCode_ = 0;

                if (WIFEXITED(status)) {
                    LOG_INFO("ProcessManager", "Child " + std::to_string(target_pid) + " exited with code " + std::to_string(lastExitCode_) + ".");
                }
                else if (WIFSIGNALED(status)) {
                    LOG_INFO("ProcessManager", "Child " + std::to_string(target_pid) + " killed by signal " + std::to_string(WTERMSIG(status)) + " (exit=" + std::to_string(lastExitCode_) + ").");
                }
                else {
                    LOG_INFO("ProcessManager", "Child " + std::to_string(target_pid) + " reaped (exit=" + std::to_string(lastExitCode_) + ").");
                }
                return true;
            }
            if (r < 0 && errno != ECHILD) {
                LOG_WARNING("ProcessManager", "waitpid transient error (errno=" + std::to_string(errno) + ").");
            }
            else if (r < 0 && errno == ECHILD) {
                lastExitCode_ = 0;
                LOG_INFO("ProcessManager", "No child to reap (ECHILD). Treating as already exited.");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        }
        return false;
        };

    auto reapAllChildrenNonBlocking = [&]() {
    int_total:
        int total = 0;
        for (;;) {
            int status = 0;
            pid_t r = waitpid(-1, &status, WNOHANG);
            if (r > 0) { ++total; continue; }
            if (r == 0 || (r < 0 && errno == ECHILD)) break;
            break;
        }
        if (total > 0) {
            LOG_INFO("ProcessManager", "Reaped " + std::to_string(total) + " additional child(ren).");
        }
        };

    // polite then sledgehammer (existing behavior)
    auto send_group = [&](int sig, const char* name) {
        int rc = kill(-target_pgid, sig);
        if (rc == -1) {
            LOG_ERROR("ProcessManager", std::string("Failed to send ") + name + " to PGID " + std::to_string(target_pgid) + " (errno=" + std::to_string(errno) + "). Falling back to PID " + std::to_string(target_pid) + ".");
            (void)kill(target_pid, sig);
        }
        else {
            LOG_INFO("ProcessManager", std::string("Sent ") + name + " to PGID " + std::to_string(target_pgid) + ".");
        }
        };

    send_group(SIGTERM, "SIGTERM");
    if (waitChildExitTimed(500)) {
        reapAllChildrenNonBlocking();
        pid_ = -1; pgid_ = -1;
        LOG_INFO("ProcessManager", "Process group terminated gracefully after SIGTERM.");
        return;
    }

    LOG_WARNING("ProcessManager", "Process group did not respond to SIGTERM. Escalating to SIGKILL.");
    send_group(SIGKILL, "SIGKILL");
    (void)waitChildExitTimed(2000);
    reapAllChildrenNonBlocking();

    pid_ = -1; pgid_ = -1;
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