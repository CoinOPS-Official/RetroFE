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
    #include <Windows.h>
#endif

#ifdef __APPLE__
struct PathHash {
    auto operator()(const std::filesystem::path& p) const noexcept {
        return std::filesystem::hash_value(p);
    }
};
#endif

class Utils
{
public:
    static std::string replace(std::string subject, const std::string_view& search,
        const std::string_view& replace);

    static float convertFloat(const std::string_view& content);
    static int convertInt(const std::string_view& content);
    static void replaceSlashesWithUnderscores(std::string& content);
#ifdef WIN32    
    static void postMessage(LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam);
#endif    
    static std::string getDirectory(const std::string& filePath);
    static std::string getParentDirectory(std::string filePath);
    static std::string getEnvVar(std::string const& key);
    static void setEnvVar(const std::string& var, const std::string& value);
    static std::string getFileName(const std::string& filePath);
    static bool findMatchingFile(const std::string& prefix, const std::vector<std::string>& extensions, std::string& file);
    static std::string toLower(const std::string& inputStr);
    static std::string uppercaseFirst(std::string str);
    static std::string filterComments(const std::string& line);
    static std::string trimEnds(const std::string& str);
    static void listToVector(const std::string& str, std::vector<std::string>& vec, char delimiter);
    static int gcd(int a, int b);
    static std::string trim(std::string& str);
    static std::string removeAbsolutePath(const std::string& fullPath);
    static bool isOutputATerminal();
    static bool startsWith(const std::string& fullString, const std::string& startOfString);
    static bool startsWithAndStrip(std::string& fullString, const std::string& startOfString);
    static std::string getOSType();
    
    template <typename... Paths>
    static std::string combinePath(Paths&&... paths) {
        std::filesystem::path combinedPath;
        // Use fold expression with perfect forwarding and direct construction
        ((combinedPath /= std::filesystem::path(std::forward<Paths>(paths))), ...);
        return combinedPath.make_preferred().string();
    }

   
#ifdef WIN32
    static const char pathSeparator = '\\';
#else
    static const char pathSeparator = '/';
#endif

private:
#ifdef __APPLE__
    static std::unordered_map<std::filesystem::path, std::unordered_set<std::string>, PathHash> fileCache;
    static std::unordered_set<std::filesystem::path, PathHash> nonExistingDirectories;
#else
    static std::unordered_map<std::filesystem::path, std::unordered_set<std::string>> fileCache;
    static std::unordered_set<std::filesystem::path> nonExistingDirectories;
#endif
    static void populateCache(const std::filesystem::path& directory);
    static bool isFileInCache(const std::filesystem::path& directory, const std::string& filename);
    static bool isFileCachePopulated(const std::filesystem::path& directory);
    
    Utils();
    virtual ~Utils();
};
