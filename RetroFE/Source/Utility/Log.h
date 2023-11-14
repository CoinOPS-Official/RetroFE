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
        ZONE_ERROR

    };
    static bool initialize(std::string file, Configuration* config);
    static bool isLevelEnabled(Zone zone);
    static void write(Zone zone, std::string component, std::string message);
    static void deInitialize();
private:
	static std::string zoneToString(Zone zone);
    static std::streambuf* cerrStream_;
    static std::streambuf* coutStream_;
    static std::ofstream writeFileStream_;
    static Configuration* config_;
};

#define LOG_DEBUG(component, message) \
    if (Logger::isLevelEnabled(Logger::ZONE_DEBUG)) \
        Logger::write(Logger::ZONE_DEBUG, component, message)

#define LOG_INFO(component, message) \
    if (Logger::isLevelEnabled(Logger::ZONE_INFO)) \
        Logger::write(Logger::ZONE_INFO, component, message)

#define LOG_NOTICE(component, message) \
    if (Logger::isLevelEnabled(Logger::ZONE_NOTICE)) \
        Logger::write(Logger::ZONE_NOTICE, component, message)

#define LOG_WARNING(component, message) \
    if (Logger::isLevelEnabled(Logger::ZONE_WARNING)) \
        Logger::write(Logger::ZONE_WARNING, component, message)

#define LOG_ERROR(component, message) \
    if (Logger::isLevelEnabled(Logger::ZONE_ERROR)) \
        Logger::write(Logger::ZONE_ERROR, component, message)
