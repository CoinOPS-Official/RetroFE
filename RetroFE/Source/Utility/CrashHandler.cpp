#include "CrashHandler.h"
#include "Utils.h"           
#include "../SDL.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring> // For strlen
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
// Instruct the compiler to link dbghelp.lib dynamically on Windows
#pragma comment(lib, "dbghelp.lib") 
#else
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>     // For low-level open()
#include <execinfo.h>  // For backtrace and backtrace_symbols_fd
#include <csignal>
#include <cerrno>
#endif

namespace fs = std::filesystem;

// Static tracking pointer to bridge native OS callbacks to our handler context safely
static CrashHandler* g_CrashHandlerInstance = nullptr;

// A fallback string to keep path generation safe if global state hasn't initialized
static std::string g_EmergencyLogPath = "log.txt";

// ------------------------------------------------------------------------
// 1. WINDOWS MINIDUMP WORKER (Moved to top so Exception Filter can see it!)
// ------------------------------------------------------------------------
#ifdef _WIN32
void createMinidump(struct _EXCEPTION_POINTERS* exceptionInfo, const std::string& baseDirectory) {
    SYSTEMTIME localTime;
    GetLocalTime(&localTime);

    std::string dumpPath = Utils::combinePath(baseDirectory, "retrofe_crash.dmp");

    HANDLE hFile = CreateFileA(
        dumpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION dumpExceptionInfo;
        dumpExceptionInfo.ThreadId = GetCurrentThreadId();
        dumpExceptionInfo.ExceptionPointers = exceptionInfo;
        dumpExceptionInfo.ClientPointers = TRUE;

        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            MiniDumpWithDataSegs,
            &dumpExceptionInfo,
            nullptr,
            nullptr
        );

        CloseHandle(hFile);
    }
}
#endif

// ------------------------------------------------------------------------
// 2. INITIALIZATION
// ------------------------------------------------------------------------
void CrashHandler::initialize() {
    static CrashHandler instance;
    g_CrashHandlerInstance = &instance;

#ifdef _WIN32
    SetUnhandledExceptionFilter(reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(windowsExceptionFilter));
#else
    std::signal(SIGSEGV, linuxSignalHandler);
    std::signal(SIGFPE, linuxSignalHandler);
    std::signal(SIGILL, linuxSignalHandler);
    std::signal(SIGABRT, linuxSignalHandler);
    std::signal(SIGBUS, linuxSignalHandler);
#endif

    try {
        fs::path baseDir;
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(NULL, buffer, MAX_PATH);
        baseDir = fs::path(buffer).parent_path();
        g_EmergencyLogPath = Utils::combinePath(baseDir.parent_path().string(), "log.txt");
#else
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';
            baseDir = fs::path(buffer).parent_path();
            g_EmergencyLogPath = Utils::combinePath(baseDir.string(), "log.txt");
        }
#endif
    }
    catch (...) {
        g_EmergencyLogPath = "log.txt";
    }
}

// ------------------------------------------------------------------------
// 3. LOG AND PRESENTATION
// ------------------------------------------------------------------------
void CrashHandler::logAndShowCrash(const std::string& errorReason, const std::string& stackTrace) {
    std::stringstream fullAlert;
    fullAlert << "RetroFE has encountered a fatal crash.\n\n"
        << "Reason: " << errorReason << "\n\n"
        << "Call Stack Context:\n" << stackTrace;

    std::ofstream logFile(g_EmergencyLogPath, std::ios::app);
    if (logFile.is_open()) {
        logFile << "\n==================================================\n";
        logFile << "[CRITICAL] [FATAL CRASH DETECTED]\n";
        logFile << fullAlert.str();
        logFile << "==================================================\n";
        logFile.flush();
        logFile.close();
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_SetWindowRelativeMouseMode(SDL::getWindow(0), false);

    std::string popupMsg = "RetroFE has encountered a fatal error (" + errorReason + ").\n\n"
        "A diagnostic binary minidump and call stack trace have been forced to your log file.\n"
        "Please send your layout configurations and log.txt to the developer.";

    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "RetroFE - Fatal Application Error",
        popupMsg.c_str(),
        nullptr
    );
}

// ------------------------------------------------------------------------
// 4. WINDOWS FILTER LAYER
// ------------------------------------------------------------------------
#ifdef _WIN32
long __stdcall CrashHandler::windowsExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo) {
    if (!g_CrashHandlerInstance) return 1;

    DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    std::string reason = "Unknown Hard Exception";

    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:   reason = "EXCEPTION_ACCESS_VIOLATION (Memory Fault / Null Pointer Dereference)"; break;
        case EXCEPTION_STACK_OVERFLOW:     reason = "EXCEPTION_STACK_OVERFLOW (Stack Exhaustion)"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: reason = "EXCEPTION_INT_DIVIDE_BY_ZERO (Math/Division Error)"; break;
    }

    // Capture target folder path one level up from the pre-calculated emergency log file
    std::filesystem::path dumpFolder = std::filesystem::path(g_EmergencyLogPath).parent_path();

    // FIXED: WE NOW EXPLICITLY CALL MINIDUMP GENERATION!
    createMinidump(exceptionInfo, dumpFolder.string());

    std::stringstream ss;
    ss << "Fault address pointer: 0x" << std::hex << reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress) << "\n";
    ss << "A binary minidump file (retrofe_crash.dmp) has been generated in your application root folder.\n\n";

    void* stackBuffer[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stackBuffer, nullptr);
    ss << "Stack Trace (" << frames << " frames captured):\n";
    for (USHORT i = 0; i < frames; ++i) {
        ss << "  [" << i << "] Frame Offset: 0x" << std::hex << reinterpret_cast<uintptr_t>(stackBuffer[i]) << "\n";
    }

    g_CrashHandlerInstance->logAndShowCrash(reason, ss.str());
    return 1;
}
// ------------------------------------------------------------------------
// 5. UNIX SIGNAL LAYER
// ------------------------------------------------------------------------
#else
namespace {
void writeAll(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t bytesWritten = write(fd, data, size);
        if (bytesWritten > 0) {
            data += bytesWritten;
            size -= static_cast<std::size_t>(bytesWritten);
            continue;
        }
        if (bytesWritten < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

template <std::size_t N>
void writeAll(int fd, const char (&text)[N]) {
    writeAll(fd, text, N - 1);
}
} // namespace

void CrashHandler::linuxSignalHandler(int signalNumber) {
    int fd = open(g_EmergencyLogPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        constexpr char header[] = "\n==================================================\n"
            "[CRITICAL] [FATAL UNIX SIGNAL DETECTED]\n"
            "Reason: ";
        writeAll(fd, header);

        switch (signalNumber) {
            case SIGSEGV: writeAll(fd, "SIGSEGV (Segmentation Fault)\n"); break;
            case SIGFPE:  writeAll(fd, "SIGFPE (Arithmetic Error)\n"); break;
            case SIGILL:  writeAll(fd, "SIGILL (Illegal Instruction)\n"); break;
            case SIGABRT: writeAll(fd, "SIGABRT (Abort Script Call)\n"); break;
            case SIGBUS:  writeAll(fd, "SIGBUS (Bus Alignment Error)\n"); break;
            default:      writeAll(fd, "Unknown System Signal\n"); break;
        }

        void* stackBuffer[64];
        int frames = backtrace(stackBuffer, 64);

        writeAll(fd, "Stack Trace Call Frames:\n");

        backtrace_symbols_fd(stackBuffer, frames, fd);

        writeAll(fd, "==================================================\n");

#ifdef __linux__
        fsync(fd);
#endif
        close(fd);
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    _exit(EXIT_FAILURE);
}
#endif
