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

#include "Utils.h"
#include "../Database/Configuration.h"
#include "Log.h"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <dirent.h>
#include <locale>
#include <list>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>

#ifdef WIN32
    #include <Windows.h>
#endif

std::unordered_map<std::filesystem::path, std::unordered_set<std::string>> Utils::fileCache;
std::unordered_set<std::filesystem::path> Utils::nonExistingDirectories;

Utils::Utils() = default;

Utils::~Utils() = default;

#ifdef WIN32
void Utils::postMessage( LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam ) {
    HWND hwnd = FindWindow(NULL, windowTitle);
	if (hwnd != NULL) {
        PostMessage(hwnd, Msg, wParam, lParam);
    }
}
#endif

std::string Utils::toLower(const std::string& inputStr)
{
    std::string str = inputStr;
    std::locale loc; // Initialize locale once
    for (auto& ch : str)
    {
        ch = std::tolower(ch, loc);
    }
    return str;
}

std::string Utils::uppercaseFirst(std::string str)
{
    if(str.length() > 0)
    {
        std::locale loc;
        str[0] = std::toupper(str[0], loc);
    }

    return str;
}
std::string Utils::filterComments(std::string line)
{
    size_t position;

    // strip out any comments
    if((position = line.find("#")) != std::string::npos)
    {
        line = line.substr(0, position);
    }
    // unix only wants \n. Windows uses \r\n. Strip off the \r for unix.
    line.erase( std::remove(line.begin(), line.end(), '\r'), line.end() );
    
    return line;
}


void Utils::populateCache(const std::filesystem::path& directory) {
    LOG_DEBUG("File Cache", "Populating cache for directory: " + directory.string());

    std::unordered_set<std::string>& files = fileCache[directory];
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            files.insert(entry.path().filename().string());
        }
    }
}

bool Utils::isFileInCache(const std::filesystem::path& baseDir, const std::string& filename) {
    auto baseDirIt = fileCache.find(baseDir);
    if (baseDirIt != fileCache.end()) {
        const auto& files = baseDirIt->second;
        if (files.find(filename) != files.end()) {
            // Logging cache hit
            LOG_DEBUG("File Cache", "Hit:  " + removeAbsolutePath(baseDir.string()) + " contains " + filename);
            return true;
        }
    }

    return false;
}



bool Utils::isFileCachePopulated(const std::filesystem::path& baseDir) {
    return fileCache.find(baseDir) != fileCache.end();
}

bool Utils::findMatchingFile(const std::string& prefix, const std::vector<std::string>& extensions, std::string& file) {

        namespace fs = std::filesystem;

        fs::path absolutePath = Configuration::convertToAbsolutePath(Configuration::absolutePath, prefix);
        fs::path baseDir = absolutePath.parent_path();

        // Check if the directory is known to not exist
        if (nonExistingDirectories.find(baseDir) != nonExistingDirectories.end()) {
            LOG_DEBUG("File Cache", "Skipping non-existing directory: " + removeAbsolutePath(baseDir.string()));
            return false; // Directory was previously found not to exist
        }

        if (!fs::is_directory(baseDir)) {
            // Handle the case where baseDir is not a directory
            nonExistingDirectories.insert(baseDir); // Add to non-existing directories cache
            return false;
        }

        std::string baseFileName = absolutePath.filename().string();

        if (!isFileCachePopulated(baseDir)) {
            populateCache(baseDir);
        }

        bool foundInCache = false;
        for (const auto& ext : extensions) {
            std::string tempFileName = baseFileName + "." + ext;
            if (isFileInCache(baseDir, tempFileName)) {
                file = (baseDir / tempFileName).string();
                foundInCache = true;
                break;
            }
        }

        if (!foundInCache) {
            // Log cache miss only once per directory after checking all extensions
            LOG_DEBUG("File Cache", "Miss: " + removeAbsolutePath(baseDir.string()) + " does not contain file '" + baseFileName + "'");
        }

        return foundInCache;

}



std::string Utils::replace(
    std::string subject,
    const std::string& search,
    const std::string& replace)
{
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos)
    {
        subject.replace(pos, search.length(), replace);
        pos += replace.length();
    }
    return subject;
}


float Utils::convertFloat(std::string content)
{
    float retVal = 0;
    std::stringstream ss;
    ss << content;
    ss >> retVal;

    return retVal;
}

int Utils::convertInt(std::string content)
{
    int retVal = 0;
    std::stringstream ss;
    ss << content;
    ss >> retVal;

    return retVal;
}

void Utils::replaceSlashesWithUnderscores(std::string &content)
{
    std::replace(content.begin(), content.end(), '\\', '_');
    std::replace(content.begin(), content.end(), '/', '_');
}


std::string Utils::getDirectory(const std::string& filePath)
{

    std::string directory = filePath;

    const size_t last_slash_idx = filePath.rfind(pathSeparator);
    if (std::string::npos != last_slash_idx)
    {
        directory = filePath.substr(0, last_slash_idx);
    }

    return directory;
}

std::string Utils::getParentDirectory(std::string directory)
{
    size_t last_slash_idx = directory.find_last_of(pathSeparator);
    if(directory.length() - 1 == last_slash_idx)
    {
        directory = directory.erase(last_slash_idx, directory.length()-1);
        last_slash_idx = directory.find_last_of(pathSeparator);
    }

    if (std::string::npos != last_slash_idx)
    {
        directory = directory.erase(last_slash_idx, directory.length());
    }

    return directory;
}

std::string Utils::getEnvVar(std::string const& key)
{
    char const* val = std::getenv(key.c_str());

    return val == NULL ? std::string() : std::string(val);
}

std::string Utils::getFileName(std::string filePath)
{

    std::string filename = filePath;

    const size_t last_slash_idx = filePath.rfind(pathSeparator);
    if (std::string::npos != last_slash_idx)
    {
        filename = filePath.erase(0, last_slash_idx+1);
    }

    return filename;
}


std::string Utils::trimEnds(std::string str)
{
    // strip off any initial tabs or spaces
    size_t trimStart = str.find_first_not_of(" \t");

    if(trimStart != std::string::npos)
    {
        size_t trimEnd = str.find_last_not_of(" \t");

        str = str.substr(trimStart, trimEnd - trimStart + 1);
    }

    return str;
}


void Utils::listToVector( const std::string& str, std::vector<std::string> &vec, char delimiter = ',' )
{
    std::string value;
    std::size_t current;
    std::size_t previous = 0;
    current = str.find( delimiter );
    while (current != std::string::npos)
    {
        value = Utils::trimEnds(str.substr(previous, current - previous));
        if (value != "") {
            vec.push_back(value);
        }
        previous = current + 1;
        current  = str.find( delimiter, previous );
    }
    value = Utils::trimEnds(str.substr(previous, current - previous));
    if (value != "") {
        vec.push_back(value);
    }
}


int Utils::gcd( int a, int b )
{
    if (b == 0)
        return a;
    return gcd( b, a % b );
}

std::string Utils::trim(std::string& str)
{
    str.erase(str.find_last_not_of(' ') + 1);         //suffixing spaces
    str.erase(0, str.find_first_not_of(' '));       //prefixing spaces
    return str;
}

std::string Utils::removeAbsolutePath(const std::string& fullPath) {
    std::string rootPath = Configuration::absolutePath; // Get the absolute path
    std::size_t found = fullPath.find(rootPath);

    if (found != std::string::npos) {
        // Remove the rootPath part from fullPath
        return fullPath.substr(0, found) + "." + fullPath.substr(found + rootPath.length());
    }
    return fullPath; // Return the original path if root is not found
}