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

#include "Log.h"
#include <iostream>
#include <sstream>
#include <ctime>
#include "../Database/Configuration.h"
#include "../Database/GlobalOpts.h"

std::ofstream Logger::writeFileStream_;
std::streambuf* Logger::cerrStream_ = NULL;
std::streambuf* Logger::coutStream_ = NULL;
std::mutex Logger::writeMutex_;
Configuration* Logger::config_ = NULL;

bool Logger::initialize(std::string file, Configuration* config)
{
    // Open the log file in truncate mode to clear it, then switch to append mode
    writeFileStream_.open(file.c_str(), std::ios::out | std::ios::trunc);
    if (!writeFileStream_.is_open()) {
        return false;
    }

    // Reopen the file in append mode for subsequent writes
    writeFileStream_.close();
    writeFileStream_.open(file.c_str(), std::ios::out | std::ios::app);

    cerrStream_ = std::cerr.rdbuf(writeFileStream_.rdbuf());
    coutStream_ = std::cout.rdbuf(writeFileStream_.rdbuf());
    config_ = config;

    return writeFileStream_.is_open();
}

void Logger::deInitialize()
{
    if (writeFileStream_.is_open()) {
        writeFileStream_.close();
    }

    std::cerr.rdbuf(cerrStream_);
    std::cout.rdbuf(coutStream_);
}


void Logger::write(Zone zone, const std::string& component, const std::string& message)
{
    std::scoped_lock lock(writeMutex_); // Ensures thread safety

    std::string zoneStr(zoneToString(zone)); // Explicit conversion from string_view to string

    std::time_t rawtime = std::time(NULL);
    struct tm const* timeinfo = std::localtime(&rawtime);

    static char timeStr[60];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);

    std::stringstream ss;
    ss << "[" << timeStr << "] [" << zoneStr << "] [" << component << "] " << message << std::endl;
    std::cout << ss.str();
    std::cout.flush();
}



#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sstream>

bool Logger::isLevelEnabled(const std::string& zone, const std::string& component) {
    static std::once_flag initFlag;
    static std::unordered_map<std::string, bool> globalFilters;
    static std::unordered_map<std::string, std::unordered_set<std::string>> categoryFilters;
    static std::unordered_map<std::string, std::unordered_set<std::string>> excludedFilters;
    static bool allowAll = false;
    static bool allowNone = false;
    static std::string level;

    if (!config_) return false;

    std::call_once(initFlag, []() {
        Logger::config_->getProperty(OPTION_LOG, level);

        std::stringstream ss(level);
        std::string token;
        while (std::getline(ss, token, ',')) {
            size_t colonPos = token.find(':');

            if (token == "ALL") {
                allowAll = true;
                return;
            }
            if (token == "NONE") {
                allowNone = true;
                return;
            }

            bool isExclusion = (token[0] == '-');
            std::string logLevel = isExclusion ? token.substr(1) : token;

            if (colonPos != std::string::npos) {
                // Split by `:` to support multiple categories per level
                std::istringstream categoryStream(token);
                std::vector<std::string> parts;
                std::string part;

                while (std::getline(categoryStream, part, ':')) {
                    parts.push_back(part);
                }

                if (parts.size() > 1) {
                    std::string logLevel = parts[0];

                    // Add all categories under this log level
                    for (size_t i = 1; i < parts.size(); i++) {
                        if (isExclusion) {
                            excludedFilters[logLevel].insert(parts[i]); // Exclude this component
                        } else {
                            categoryFilters[logLevel].insert(parts[i]); // Enable this component
                        }
                    }
                }
            } else {
                // This is a global log level, e.g., "DEBUG", "ERROR"
                if (isExclusion) {
                    globalFilters.erase(logLevel); // Remove from global filters
                } else {
                    globalFilters[logLevel] = true;
                }
            }
        }
        });

    // If ALL is enabled, always return true unless explicitly excluded
    if (allowAll) {
        auto excludedIt = excludedFilters.find(zone);
        if (excludedIt != excludedFilters.end()) {
            if (excludedIt->second.find(component) != excludedIt->second.end()) {
                return false; // Explicitly excluded component
            }
        }
        return true;
    }

    // If NONE is set, always return false
    if (allowNone) {
        return false;
    }

    // Check if the log level is explicitly disabled for this category
    auto excludedIt = excludedFilters.find(zone);
    if (excludedIt != excludedFilters.end()) {
        if (excludedIt->second.find(component) != excludedIt->second.end()) {
            return false; // Explicitly disabled
        }
    }

    // Check if the log level is globally enabled
    if (globalFilters.find(zone) != globalFilters.end()) {
        return true;
    }

    // Check if the log level is enabled for the specific component
    auto it = categoryFilters.find(zone);
    if (it != categoryFilters.end()) {
        if (it->second.find(component) != it->second.end()) {
            return true;  // Component-specific log level is enabled
        }
    }

    return false;
}

constexpr std::string_view Logger::zoneToString(Zone zone)
{
    switch (zone) {
    case ZONE_INFO: return "INFO";
    case ZONE_DEBUG: return "DEBUG";
    case ZONE_NOTICE: return "NOTICE";
    case ZONE_WARNING: return "WARNING";
    case ZONE_ERROR: return "ERROR";
    case ZONE_FILECACHE: return "FILECACHE";
    default: return "UNKNOWN";
    }
}