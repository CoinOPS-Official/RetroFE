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

std::ofstream Logger::writeFileStream_;
std::streambuf* Logger::cerrStream_ = NULL;
std::streambuf* Logger::coutStream_ = NULL;
Configuration* Logger::config_ = NULL;

bool Logger::initialize(std::string file, Configuration* config)
{
    writeFileStream_.open(file.c_str());

    cerrStream_ = std::cerr.rdbuf(writeFileStream_.rdbuf());
    coutStream_ = std::cout.rdbuf(writeFileStream_.rdbuf());
    config_ = config;

    return writeFileStream_.is_open();
}

void Logger::deInitialize()
{
    if (writeFileStream_.is_open())
    {
        writeFileStream_.close();

    }

    std::cerr.rdbuf(cerrStream_);
    std::cout.rdbuf(coutStream_);
}


void Logger::write(Zone zone, std::string component, std::string message)
{
    std::string zoneStr = zoneToString(zone);

    std::time_t rawtime = std::time(NULL);
    struct tm const* timeinfo = std::localtime(&rawtime);

    static char timeStr[60];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);

    std::stringstream ss;
    ss << "[" << timeStr << "] [" << zoneStr << "] [" << component << "] " << message << std::endl;
    std::cout << ss.str();
    std::cout.flush();
}


bool Logger::isLevelEnabled(Zone zone) {
    if (!config_) return false; // do not log if log in settings.conf is not set
    
    static bool isInitialized = false;
    static std::string level;
      
    if (!isInitialized) {
        Logger::config_->getProperty("log", level);
        isInitialized = true;
    }

    if (level == "ALL") return true;

    std::string zoneStr = zoneToString(zone);
    std::stringstream ss(level);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token == zoneStr) {
            return true;
        }
    }
    return false;
}


std::string Logger::zoneToString(Zone zone)
{
    switch (zone)
    {
    case ZONE_INFO:
        return "INFO";
    case ZONE_DEBUG:
        return "DEBUG";
    case ZONE_NOTICE:
        return "NOTICE";
    case ZONE_WARNING:
        return "WARNING";
    case ZONE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}