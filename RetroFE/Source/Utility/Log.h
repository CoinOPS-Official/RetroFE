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
#include <fstream>
#include <sstream>
#include <streambuf>
#include <iostream>
#include <mutex>
#include "../Database/Configuration.h"

class Logger
{
public:
    enum Zone
    {
        ZONE_DEBUG,
        ZONE_INFO,
        ZONE_NOTICE,
        ZONE_WARNING,
        ZONE_ERROR,
        ZONE_FILECACHE
    };
    static bool initialize(std::string file, Configuration* config);
    static void write(Zone zone, const std::string& component, const std::string& message);
    static bool isLevelEnabled(const std::string& zone, const std::string& component);
    static constexpr std::string_view zoneToString(Zone zone);
    static void deInitialize();
private:
    static std::streambuf* cerrStream_;
    static std::streambuf* coutStream_;
    static std::ofstream writeFileStream_;
    static Configuration* config_;
    static std::mutex writeMutex_;
};

#define LOG_DEBUG(component, message) \
    if (Logger::isLevelEnabled("DEBUG", component)) { \
        std::ostringstream oss; \
        oss << message; \
        Logger::write(Logger::ZONE_DEBUG, component, oss.str()); \
    }

#define LOG_INFO(component, message) \
    if (Logger::isLevelEnabled("INFO", component)) { \
        std::ostringstream oss; \
        oss << message; \
        Logger::write(Logger::ZONE_INFO, component, oss.str()); \
    }

#define LOG_NOTICE(component, message) \
    if (Logger::isLevelEnabled("NOTICE", component)) { \
        std::ostringstream oss; \
        oss << message; \
        Logger::write(Logger::ZONE_NOTICE, component, oss.str()); \
    }

#define LOG_WARNING(component, message) \
    if (Logger::isLevelEnabled("WARNING", component)) { \
        std::ostringstream oss; \
        oss << message; \
        Logger::write(Logger::ZONE_WARNING, component, oss.str()); \
    }

#define LOG_ERROR(component, message) \
    if (Logger::isLevelEnabled("ERROR", component)) { \
        std::ostringstream oss; \
        oss << message; \
        Logger::write(Logger::ZONE_ERROR, component, oss.str()); \
    }

#define LOG_FILECACHE(component, message) \
    if (Logger::isLevelEnabled("FILECACHE", component)) { \
        std::ostringstream oss; \
        oss << message; \
        Logger::write(Logger::ZONE_FILECACHE, component, oss.str()); \
    }

