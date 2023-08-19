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
#include <string_view>

#ifdef WIN32
    #define NOMINMAX
    #include <windows.h>
#endif

class Utils
{
public:
    static std::string replace(std::string subject, const std::string& search,
                               const std::string& replace);

    static float convertFloat(const std::string& content);
    static int convertInt(const std::string& content);
    static void replaceSlashesWithUnderscores(std::string &content);
#ifdef WIN32    
    static void postMessage(LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam );
#endif    
    static std::string getDirectory(const std::string& filePath);
    static std::string getParentDirectory(const std::string& directory);
    static std::string getEnvVar(std::string const& key);
    static std::string getFileName(const std::string& filePath);
    static bool findMatchingFile(const std::string& prefix, const std::vector<std::string>& extensions, std::string& file);
    static std::string toLower(const std::string& str);
    static std::string uppercaseFirst(const std::string& str);
    static std::string filterComments(const std::string& line);
    static std::string_view trimEnds(std::string_view str);
    static void listToVector(const std::string& str, std::vector<std::string>& vec, char delimiter = ',');
    static int gcd( int a, int b );
    static std::string trim(std::string& str);

    // Base function to handle a list of paths
    static std::string combinePath(const std::list<std::string>& paths);

    // Variadic template function to handle any number of string arguments
    template <typename... Strings>
    static std::string combinePath(Strings... paths) {
        return combinePath(std::list<std::string>{paths...});
    }
   
private:
    Utils();
    virtual ~Utils();
};

