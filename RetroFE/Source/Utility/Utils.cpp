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
#include <locale>
#include <list>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <codecvt>
#include <thread>
#include <cmath>
#ifndef __APPLE__
#include <charconv>
#endif

#ifdef WIN32
#include <Windows.h>
#endif

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

// Initialize the static member variables
#ifdef __APPLE__
std::unordered_map<std::filesystem::path, std::unordered_set<std::string>, PathHash> Utils::fileCache;
std::unordered_set<std::filesystem::path, PathHash> Utils::nonExistingDirectories;
#else
std::unordered_map<std::filesystem::path, std::unordered_set<std::string>> Utils::fileCache;
std::unordered_set<std::filesystem::path> Utils::nonExistingDirectories;
#endif

Utils::Utils() = default;

Utils::~Utils() = default;

#ifdef WIN32
void Utils::postMessage( LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam ) {
    HWND hwnd = FindWindow(NULL, windowTitle);
    if (hwnd != NULL) {
        PostMessage(hwnd, Msg, wParam, lParam);
    }
}

// Utility function to convert std::wstring to std::string
std::string Utils::wstringToString(const std::wstring& wstr) {
    if (wstr.empty()) {
        return std::string();
    }
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
#endif

void Utils::preciseSleep(double seconds_to_sleep) {
    using namespace std::chrono;

    if (seconds_to_sleep <= 0.0) {
        return;
    }

    // Each thread gets its own adaptive parameters for preciseSleep
    thread_local static double estimate_s = 0.010; // Initial estimate in seconds (e.g., 10ms)
    thread_local static double mean_s = 0.010; // Initial mean in seconds
    thread_local static double m2_s = 0.0;   // For variance calculation
    thread_local static int64_t count = 1;

    double remaining_seconds = seconds_to_sleep;

    // Note: This adaptive loop is based on the idea of repeatedly sleeping for 1ms
    // and learning. If seconds_to_sleep is small (e.g., < estimate_s), it goes straight to spin.
    // The effectiveness of this adaptive part vs. a simpler (sleep_for(total - fudge) then spin)
    // depends on the OS and typical sleep durations.
    while (remaining_seconds > estimate_s) {
        auto start_chunk = steady_clock::now();
        std::this_thread::sleep_for(milliseconds(1)); // Coarse sleep for 1ms
        auto end_chunk = steady_clock::now();

        double observed_s = duration<double>(end_chunk - start_chunk).count();
        remaining_seconds -= observed_s;

        // Update running statistics (Welford's algorithm)
        ++count;
        double delta_s = observed_s - mean_s;
        mean_s += delta_s / count;
        m2_s += delta_s * (observed_s - mean_s);

        double stddev_s = 0.0;
        if (count > 1) { // Need at least 2 samples for stddev
            stddev_s = std::sqrt(m2_s / (count - 1));
        }
        estimate_s = mean_s + stddev_s; // Update estimate for next iteration
    }

    // Spin for the remainder (or if seconds_to_sleep was < estimate_s initially)
    // Ensure remaining_seconds for spin isn't negative if coarse sleep overshot.
    if (remaining_seconds > 0.0) {
        auto spin_start_time = steady_clock::now();
        // Create duration object once
        auto spin_delay_duration = duration<double>(remaining_seconds);
        while (steady_clock::now() - spin_start_time < spin_delay_duration) {
            // Spin
            // Consider adding _mm_pause() or std::this_thread::yield() for very long spins,
            // but for typical sub-millisecond final spins, pure spin is often best.
        }
    }
}

std::string Utils::toLower(const std::string& inputStr)
{
    std::string str = inputStr;
    for (auto& ch : str) {
        // Explicitly cast the result of std::tolower back to char
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return str;
}

std::string Utils::uppercaseFirst(std::string str)
{
    if(str.length() > 0) {
        std::locale loc;
        str[0] = std::toupper(str[0], loc);
    }

    return str;
}

std::string Utils::filterComments(const std::string& line) {
    // Use string_view to efficiently find the comment position
    std::string_view lineView(line);
    size_t position = lineView.find("#");
    if (position != std::string_view::npos) {
        // Narrow down the view to exclude the comment
        lineView = lineView.substr(0, position);
    }

    // Convert the string_view back to a string to perform modifications
    std::string result(lineView.begin(), lineView.end());

    // Remove carriage return characters from the string
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());

    return result;
}


void Utils::populateCache(const std::filesystem::path& directory) {
    LOG_FILECACHE("Populate", "Populating cache for directory: " + directory.string());

    std::unordered_set<std::string>& files = fileCache[directory];
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
#ifdef WIN32
            files.insert(Utils::toLower(entry.path().filename().string()));
#else
            files.insert(entry.path().filename().string());
#endif
        }
    }
}

bool Utils::isFileInCache(const std::filesystem::path& baseDir, const std::string& filename) {
    auto baseDirIt = fileCache.find(baseDir);
    if (baseDirIt != fileCache.end()) {
        const auto& files = baseDirIt->second;
#ifdef WIN32
        if (files.find(Utils::toLower(filename)) != files.end()) {
#else
        if (files.find(filename) != files.end()) {
#endif
            // Logging cache hit
            LOG_FILECACHE("Hit", removeAbsolutePath(baseDir.string()) + " contains " + filename);
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

    fs::path absolutePath = Utils::combinePath(Configuration::absolutePath, prefix);
    fs::path baseDir = absolutePath.parent_path();

    // Check if the directory is known to not exist
    if (nonExistingDirectories.find(baseDir) != nonExistingDirectories.end()) {
        LOG_FILECACHE("Skip", "Skipping non-existing directory: " + removeAbsolutePath(baseDir.string()));
        return false; // Directory was previously found not to exist
    }

    // Early exit if the directory doesn't exist
    if (!fs::is_directory(baseDir)) {
        nonExistingDirectories.insert(baseDir); // Add to non-existing directories cache
        return false;
    }

    std::string baseFileName = absolutePath.filename().string();

    // Ensure the cache is populated
    if (!isFileCachePopulated(baseDir)) {
        populateCache(baseDir);
    }

    // Check for the file with each extension in the cache
    for (const auto& ext : extensions) {
        std::string tempFileName = baseFileName + "." + ext;
        if (isFileInCache(baseDir, tempFileName)) {
            file = (baseDir / tempFileName).string();
            return true; // File found
        }
    }

    // Log cache miss after checking all extensions
    LOG_FILECACHE("Miss", removeAbsolutePath(baseDir.string()) + " does not contain file '" + baseFileName + "'");
    return false; // No matching file found
}


std::string Utils::replace(
    std::string subject,
    const std::string_view& search,
    const std::string_view& replace)
{
    if (search.empty())
        return subject; // Early exit if search string is empty

    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos)
    {
        subject.replace(pos, search.length(), replace);
        pos += replace.length();
    }
    return subject;
}


float Utils::convertFloat(const std::string_view& content) {
    float retVal = 0;
#ifdef __APPLE__
    std::stringstream ss;
    ss << content;
    ss >> retVal;
#else
    std::from_chars_result result = std::from_chars(content.data(), content.data() + content.size(), retVal, std::chars_format::general);
    if (result.ec == std::errc::invalid_argument || result.ec == std::errc::result_out_of_range) {
        retVal = 0.0f; // Handle error or set default value
    }
#endif
    return retVal;
}

int Utils::convertInt(const std::string_view& content) {
    int retVal = 0;
#ifdef __APPLE__
    std::stringstream ss;
    ss << content;
    ss >> retVal;
#else
    std::from_chars_result result = std::from_chars(content.data(), content.data() + content.size(), retVal);
    if (result.ec == std::errc::invalid_argument || result.ec == std::errc::result_out_of_range) {
        // Handle error or set default value
        retVal = 0;
    }
#endif
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
    if (std::string::npos != last_slash_idx) {
        directory = filePath.substr(0, last_slash_idx);
    }

    return directory;
}

std::string Utils::getParentDirectory(std::string directory) 
{
    size_t last_slash_idx = directory.find_last_of(pathSeparator);
    if (directory.length() - 1 == last_slash_idx) {
        directory = directory.erase(last_slash_idx, directory.length() - 1);
        last_slash_idx = directory.find_last_of(pathSeparator);
    }

    if (std::string::npos != last_slash_idx) {
        directory = directory.erase(last_slash_idx, directory.length());
    }

    // If the directory ends with a drive letter (e.g., "C:"), append a backslash to form a valid root path
    if (directory.length() == 2 && directory[1] == ':') {
        directory += pathSeparator;
    }

    return directory;
}

std::string Utils::getEnvVar(std::string const& key)
{
    char const* val = std::getenv(key.c_str());

    return val == NULL ? std::string() : std::string(val);
}

void Utils::setEnvVar(const std::string& var, const std::string& value) {
#ifdef _WIN32
    // On Windows, use _putenv_s
    if (_putenv_s(var.c_str(), value.c_str()) != 0) {
        LOG_ERROR("Utils", "Failed to set GST_DEBUG_DUMP_DOT_DIR environment variable.");
    }
#else
    // On Unix-like systems, use setenv
    if (setenv(var.c_str(), value.c_str(), 1) != 0) {
        LOG_ERROR("Utils", "Failed to set GST_DEBUG_DUMP_DOT_DIR environment variable.");
    }
#endif
}

std::string Utils::getFileName(const std::string& filePath) {
    return std::filesystem::path(filePath).filename().string();
}


std::string Utils::trimEnds(const std::string& str) {
    std::string_view strView = str;

    // Find the first and last characters not of tabs or spaces
    size_t trimStart = strView.find_first_not_of(" \t");
    size_t trimEnd = strView.find_last_not_of(" \t");

    // If no non-space/tab characters are found, return an empty string
    if (trimStart == std::string_view::npos) return "";

    // Otherwise, create a substring view of the trimmed section
    std::string_view trimmedView = strView.substr(trimStart, trimEnd - trimStart + 1);

    // Return a string constructed from the trimmed view
    return std::string(trimmedView.begin(), trimmedView.end());
}


void Utils::listToVector( const std::string& str, std::vector<std::string> &vec, char delimiter = ',' )
{
    std::string value;
    std::size_t current;
    std::size_t previous = 0;
    current = str.find( delimiter );
    while (current != std::string::npos) {
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

// Check if we're starting retrofe from terminal on Win or Unix
bool Utils::isOutputATerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(STDOUT_FILENO);
#endif
}

// Check if start of fullString contains startOfString
bool Utils::startsWith(const std::string& fullString, const std::string& startOfString) {
    return fullString.substr(0, startOfString.length()) == startOfString;
}

// Check if start of fullString contains startOfString and then remove
bool Utils::startsWithAndStrip(std::string& fullString, const std::string& startOfString) {
    if (fullString.substr(0, startOfString.length()) == startOfString) {
        fullString = fullString.substr(startOfString.length());
        return true;
    }
    return false;
}


std::string Utils::getOSType(){
#ifdef WIN32
    std::string osType = "windows";
#elif __APPLE__
    std::string osType = "apple";
#else
    std::string osType = "linux";
#endif
    return osType;
}

// Define the obfuscation key (simple and hard-coded for now)
const std::string Utils::obfuscationKey = "s3cReT123!";

// Public method to obfuscate data using XOR with the key
std::string Utils::obfuscate(const std::string& data) {
    return xorOperation(data, obfuscationKey);
}

// Public method to de-obfuscate data (same as obfuscate due to XOR symmetry)
std::string Utils::deobfuscate(const std::string& data) {
    return xorOperation(data, obfuscationKey);
}

// Private helper method to apply XOR obfuscation
std::string Utils::xorOperation(const std::string& data, const std::string& key) {
    std::string result = data;
    for (size_t i = 0; i < data.size(); ++i) {
        result[i] = data[i] ^ key[i % key.size()];
    }
    return result;
}

std::string Utils::removeNullCharacters(const std::string& input) {
    std::string output;
    output.reserve(input.size());  // Reserve space for efficiency
    for (char ch : input) {
        if (ch != '\0') {  // Filter out null characters
            output += ch;
        }
    }
    return output;
}