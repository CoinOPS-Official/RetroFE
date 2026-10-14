#pragma once
#include <cstdlib>
#include <cstring>

// Set before launching RetroFE. Disabled runs do not collect timestamps.
inline bool videoStartupTimingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RETROFE_VIDEO_STARTUP_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
