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
std::mutex Logger::writeMutex_;
Configuration* Logger::config_ = NULL;

bool Logger::initialize(std::string file, Configuration* config)
{
    // Open the log file in truncate mode to clear it, then switch to append mode
    writeFileStream_.open(file.c_str(), std::ios::out | std::ios::trunc);
    if (!writeFileStream_.is_open())
    {
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
    if (writeFileStream_.is_open())
    {
        writeFileStream_.close();

    }

    std::cerr.rdbuf(cerrStream_);
    std::cout.rdbuf(coutStream_);
}


void Logger::write(Zone zone, const std::string& component, const std::string& message)
{
    std::lock_guard<std::mutex> guard(writeMutex_); // Locks the mutex here
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


bool Logger::isLevelEnabled(const std::string& zone) {
    static bool isInitialized = false;
    static bool isDebugEnabled = false;
    static bool isInfoEnabled = false;
    static bool isNoticeEnabled = false;
    static bool isWarningEnabled = false;
    static bool isErrorEnabled = false;
	static bool isFileCacheEnabled = false;
    static std::string level;

    if (!config_) return false;

    if (!isInitialized) {
        Logger::config_->getProperty("log", level);
        isInitialized = true;

        std::stringstream ss(level);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token == "DEBUG") isDebugEnabled = true;
            else if (token == "INFO") isInfoEnabled = true;
            else if (token == "NOTICE") isNoticeEnabled = true;
            else if (token == "WARNING") isWarningEnabled = true;
            else if (token == "ERROR") isErrorEnabled = true;
			else if (token == "FILECACHE") isFileCacheEnabled = true;
            else if (token == "ALL") {
                isDebugEnabled = isInfoEnabled = isNoticeEnabled = isWarningEnabled = isErrorEnabled = isFileCacheEnabled = true;
                break;
            }
        }
    }

    if (zone == "DEBUG") return isDebugEnabled;
    else if (zone == "INFO") return isInfoEnabled;
    else if (zone == "NOTICE") return isNoticeEnabled;
    else if (zone == "WARNING") return isWarningEnabled;
    else if (zone == "ERROR") return isErrorEnabled;
    else if (zone == "FILECACHE") return isFileCacheEnabled;

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
    case ZONE_FILECACHE:
		return "FILECACHE";
    default:
        return "UNKNOWN";
    }
}