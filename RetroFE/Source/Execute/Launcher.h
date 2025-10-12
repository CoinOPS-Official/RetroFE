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
#include "../Graphics/Page.h"
#include <atomic>
#ifdef WIN32
#include <Windows.h>
#endif

class Configuration;
class Item;
class RetroFE;

class Launcher
{
public:
    explicit Launcher(Configuration &c, RetroFE &retroFe);
    bool run(std::string collection, Item *collectionItem, Page *currentPage = NULL, bool isAttractMode = false);
    void startScript();
    void exitScript();
	void LEDBlinky( int command, std::string collection = "", Item *collectionItem = NULL);

private:
    bool runHookNoWait_(const std::string& exe, const std::string& args, const std::string& cwd);
    bool launcherName(std::string &launcherName, std::string collection);
    bool extensions(std::string &extensions, std::string launcherName);
    bool collectionDirectory(std::string &directory, std::string collection);
    bool findFile(std::string& foundFilePath, std::string& foundFilename, const std::string& directory, const std::string& filenameWithoutExtension, const std::string& extensions);
    Configuration &config_;
    RetroFE &retroFeInstance_;
};
