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
#include <string_view>


#ifdef WIN32
    #include <Windows.h>
#endif

Utils::Utils()
{
}

Utils::~Utils()
{
}

#ifdef WIN32
void Utils::postMessage( LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam ) {
    HWND hwnd = FindWindow(NULL, windowTitle);
	if (hwnd != NULL) {
        PostMessage(hwnd, Msg, wParam, lParam);
    }
}
#endif

std::string Utils::toLower(const std::string& input)
{
    std::string result = input;
    std::locale loc;

    for(char& c : result)
    {
        c = std::tolower(c, loc);
    }

    return result;
}


std::string Utils::uppercaseFirst(const std::string& input)
{
    if(input.empty()) 
        return input; // Return early if string is empty

    std::string result = input;
    result[0] = std::toupper(result[0], std::locale());

    return result;
}


std::string Utils::filterComments(const std::string& line)
{
    std::string result = line;

    // Find the first occurrence of '#'
    size_t position = result.find('#');
    if (position != std::string::npos)
    {
        // Erase the comment part
        result.erase(position);
    }

    // Remove all '\r' characters
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
    
    return result;
}


std::string Utils::combinePath(const std::list<std::string>& paths)
{
    std::filesystem::path result;
    for (const auto& p : paths)
    {
        result /= p;
    }
    return result.string();
}


bool Utils::findMatchingFile(const std::string& prefix, const std::vector<std::string>& extensions, std::string& file)
{
    for(const auto& ext : extensions)
    {
        std::string temp = prefix + "." + ext;
        temp = Configuration::convertToAbsolutePath(Configuration::absolutePath, temp);

        if(std::filesystem::exists(temp)) 
        {
            file = temp;
            return true;
        }
    }
    return false;
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


float Utils::convertFloat(const std::string& content)
{
    return std::stof(content);
}

int Utils::convertInt(const std::string& content) {
    return std::stoi(content);
}

void Utils::replaceSlashesWithUnderscores(std::string &content)
{
    std::replace(content.begin(), content.end(), '\\', '_');
    std::replace(content.begin(), content.end(), '/', '_');
}


std::string Utils::getDirectory(const std::string& filePath)
{
    std::filesystem::path path(filePath);
    return path.parent_path().string();
}


std::string Utils::getParentDirectory(const std::string& directory)
{
    std::filesystem::path dirPath(directory);
    return dirPath.parent_path().string();
}

std::string Utils::getEnvVar(std::string const& key)
{
    char const* val = std::getenv(key.c_str());

    return val == NULL ? std::string() : std::string(val);
}

std::string Utils::getFileName(const std::string& filePath)
{
    std::filesystem::path path(filePath);
    return path.filename().string();
}


std::string_view Utils::trimEnds(std::string_view view)
{
    // Find the start of the non-space characters
    size_t trimStart = view.find_first_not_of(" \t");
    if (trimStart == std::string_view::npos) {
        return ""; // View contains only spaces/tabs, return an empty view
    }

    // Find the end of the non-space characters
    size_t trimEnd = view.find_last_not_of(" \t");

    // Return a subview that represents the trimmed string
    return view.substr(trimStart, trimEnd - trimStart + 1);
}


void Utils::listToVector(const std::string& str, std::vector<std::string>& vec, char delimiter)
{
    std::string_view view(str);  // Create a view of the string
    size_t previous = 0;
    size_t current;

    while ((current = view.find(delimiter, previous)) != std::string_view::npos)
    {
        auto subview = view.substr(previous, current - previous);
        auto trimmed = trimEnds(subview);
        if (!trimmed.empty()) {
            vec.emplace_back(trimmed);  // Directly construct the string in the vector
        }
        previous = current + 1;
    }

    auto trimmed = trimEnds(view.substr(previous));
    if (!trimmed.empty()) {
        vec.emplace_back(trimmed);
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