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
    static void replaceSlashesWithUnderscores(std::string &content);
#ifdef WIN32    
    static void postMessage(LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam );
#endif    
    static std::string getDirectory(std::string filePath);
    static std::string getParentDirectory(std::string filePath);
    static std::string getEnvVar(std::string const& key);
    static std::string getFileName(std::string filePath);
    static bool findMatchingFile(std::string prefix, std::vector<std::string> &extensions, std::string &file);
    static std::string toLower(std::string str);
    static std::string uppercaseFirst(std::string str);
    static std::string filterComments(std::string line);
    static std::string trimEnds(std::string str);
    static void listToVector(const std::string& str, std::vector<std::string>& vec, char delimiter);
    static int gcd( int a, int b );
    static std::string trim(std::string& str);

    template<typename... Paths>
    static std::string combinePath(Paths... paths) {
        std::list<std::string> pathsList = { paths... };
        // Call the internal helper function to avoid recursive ambiguity
        return combinePathList(pathsList);
    }
   
#ifdef WIN32
    static const char pathSeparator = '\\';
#else
    static const char pathSeparator = '/';
#endif

private:
    Utils();
    virtual ~Utils();

    static std::string combinePathList(std::list<std::string>& paths) {
        std::string combinedPath;
        auto it = paths.begin();
        if (it != paths.end()) {
            combinedPath += *it;
            ++it;
        }
        while (it != paths.end()) {
            combinedPath += pathSeparator + *it;
            ++it;
        }
        return combinedPath;
    }
};
