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
#include <vector>
#include <list>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#ifdef WIN32
    #define NOMINMAX
    #include <windows.h>
#endif

class Utils
{
public:
    static std::string replace(std::string subject, const std::string& search,
        const std::string& replace);

    static float convertFloat(std::string content);
    static int convertInt(std::string content);
    static void replaceSlashesWithUnderscores(std::string& content);
#ifdef WIN32    
    static void postMessage(LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam);
#endif    
    static std::string getDirectory(const std::string& filePath);
    static std::string getParentDirectory(std::string filePath);
    static std::string getEnvVar(std::string const& key);
    static std::string getFileName(std::string filePath);
    static bool findMatchingFile(const std::string& prefix, const std::vector<std::string>& extensions, std::string& file);
    static std::string toLower(const std::string& inputStr);
    static std::string uppercaseFirst(std::string str);
    static std::string filterComments(std::string line);
    static std::string trimEnds(std::string str);
    static void listToVector(const std::string& str, std::vector<std::string>& vec, char delimiter);
    static int gcd(int a, int b);
    static std::string trim(std::string& str);

    template <typename... Paths>
    static std::string combinePath(Paths... paths) {
        std::filesystem::path combinedPath;
        // Use fold expression to append paths
        (combinedPath /= ... /= std::filesystem::path(paths));
        return combinedPath.make_preferred().string(); // Convert to the preferred path format for the platform
    }

   
#ifdef WIN32
    static const char pathSeparator = '\\';
#else
    static const char pathSeparator = '/';
#endif

private:
    static std::unordered_map<std::filesystem::path, std::unordered_set<std::string>> fileCache;
    static std::unordered_set<std::filesystem::path> nonExistingDirectories; // Cache for non-existing directories
    static void populateCache(const std::filesystem::path& directory);
    static bool isFileInCache(const std::filesystem::path& directory, const std::string& filename);
    static bool isFileCachePopulated(const std::filesystem::path& directory);
    
    Utils();
    virtual ~Utils();
};
