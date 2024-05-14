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

//**************************************************************************
//  GLOBAL SETTINGS OPTIONS
//**************************************************************************

#include "GlobalOpts.h"
#include "../Utility/Utils.h"
#include "Configuration.h"
#include <fstream>
#include <iostream>
#include <string>
#include <iomanip>

const global_options::options_entry global_options::s_option_entries[] =
{
    
    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "LOGGING OPTIONS" },
    { OPTION_LOG,                     "NONE",      global_options::option_type::STRING,   "Set logging level, any combo of ERROR,INFO,NOTICE,WARNING,DEBUG,FILECACHE or ALL or NONE" },
    { OPTION_DUMPPROPERTIES,          "false",     global_options::option_type::BOOLEAN,  "Dump contents of properties to txt in current directory"},

    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "DISPLAY OPTIONS" },
    { OPTION_NUMSCREENS,              "1",         global_options::option_type::INTEGER,  "Defines the number of monitors used" },
    { OPTION_FULLSCREEN,              "true",      global_options::option_type::BOOLEAN,  "Run the frontend in fullscreen" },
    { OPTION_HORIZONTAL,              "stretch",   global_options::option_type::STRING,   "Pixel width INT or STRETCH" },
    { OPTION_VERTICAL,                "stretch",   global_options::option_type::STRING,   "Pixel height INT or STRETCH" },
    { OPTION_FULLSCREENX,             "true",      global_options::option_type::BOOLEAN,  "Run the frontend in fullscreen for monitor x" },
    { OPTION_HORIZONTALX,             "",          global_options::option_type::INTEGER,  "Pixel width for monitor x" },
    { OPTION_VERTICALX,               "",          global_options::option_type::INTEGER,  "Pixel height for monitor x" },
    { OPTION_SCREENNUMX,              "",          global_options::option_type::INTEGER,  "Define which monitor x is which display window, Screen numbers start at 0!"},
    { OPTION_MIRRORX,                 "false",     global_options::option_type::BOOLEAN,  "Divides monitor x into two halves" },
    { OPTION_ROTATIONX,               "0",         global_options::option_type::INTEGER,  "Rotation of monitor x (0, 1, 2, 3)" },

    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "WINDOW OPTIONS" },
    { OPTION_WINDOWBORDER,            "false",     global_options::option_type::BOOLEAN,  "Show window border" },
    { OPTION_WINDOWRESIZE,            "false",     global_options::option_type::BOOLEAN,  "Allow window to be resized" },
    { OPTION_FPS,                     "60",        global_options::option_type::INTEGER,  "Requested FPS while in an active state" },
    { OPTION_FPSIDLE,                 "60",        global_options::option_type::INTEGER,  "Request FPS while in an idle state" },
    { OPTION_HIDEMOUSE,               "true",      global_options::option_type::BOOLEAN,  "Defines whether the mouse cursor is hidden" },
    { OPTION_ANIMATEDURINGGAME,       "true",      global_options::option_type::BOOLEAN,  "Pause animated marquees while in the game" },

    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "VIDEO OPTIONS" },
    { OPTION_VIDEOENABLE,             "true",      global_options::option_type::BOOLEAN,  "Defines whether video is rendered" },
    { OPTION_VIDEOLOOP,               "0",         global_options::option_type::INTEGER,  "Number of times to play video, 0 forever" },
    { OPTION_DISABLEVIDEORESTART,     "false",     global_options::option_type::BOOLEAN,  "Pauses video while scrolling" },
    { OPTION_DISABLEPAUSEONSCROLL,    "false",     global_options::option_type::BOOLEAN,  "Restart video when selected" },

    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "RENDERER OPTIONS" },
    { OPTION_VSYNC,                    "false",    global_options::option_type::BOOLEAN,  "Vertical synchronization" },
    { OPTION_HARDWAREVIDEOACCEL,       "false",    global_options::option_type::BOOLEAN,  "Hardware decoding" },
    { OPTION_AVDECMAXTHREADS,          "2",        global_options::option_type::INTEGER,  "Number of threads for avdec software decoding" },
    { OPTION_MUTEVIDEO,                "false",    global_options::option_type::BOOLEAN,  "Video playback is muted" },
    { OPTION_SDLRENDERDRIVER,          "direct3d", global_options::option_type::STRING,   "Set renderer (direct3d, direct3d11, direct3d12, opengl, opengles2, opengles, metal, and software)" },
    { OPTION_SCALEQUALITY,             "1",        global_options::option_type::INTEGER,  "Scaling quality (0, 1, 2)" },
    { OPTION_HIGHPRIORITY,             "false",    global_options::option_type::BOOLEAN,  "RetroFE Windows process priority" },
    { OPTION_UNLOADSDL,                "false",    global_options::option_type::BOOLEAN,  "Close SDL when launching a game, MUST be true for RPI" },
    { OPTION_MINIMIZEONFOCUSLOSS,      "false",    global_options::option_type::BOOLEAN,  "Minimize RetroFE when focus is lost" },
    { OPTION_AVDECTHREADTYPE,          "2",        global_options::option_type::INTEGER,  "Type of threading in the case of software decoding (1=frame, 2=slice)" },
    { OPTION_GLSWAPINTERVAL,           "1",        global_options::option_type::INTEGER,  "OpenGL Swap Interval (0=immediate updates, 1=synchronized vsync, -1=adaptive vsync" },

    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "CUSTOMIZATION OPTIONS" },
    { OPTION_LAYOUT,                   "Arcades",  global_options::option_type::STRING,   "Theme to be used in RetroFE, a folder name in /layouts" },
    { OPTION_RANDOMLAYOUT,             "",         global_options::option_type::MSTRING,  "Randomly choose a layout on launch, CSV list of layout names" },
    { OPTION_FIRSTPLAYLIST,            "arcades",  global_options::option_type::STRING,   "Start on this playlist if available" },
    { OPTION_AUTOPLAYLIST,             "all",      global_options::option_type::STRING,   "Start on this playlist when entering a collection if available" },
    { OPTION_CYCLEPLAYLIST,            "",         global_options::option_type::MSTRING,  "Set of playlists that can be cycled through, CSV list of playlist names" },
    { OPTION_FIRSTCOLLECTION,          "",         global_options::option_type::STRING,   "Start on this collection if available" },
    { OPTION_CYCLECOLLECTION,          "",         global_options::option_type::MSTRING,  "Set of collections that can be cycled through, CSV list of collection names" },
    { OPTION_LASTPLAYEDSIZE,           "10",       global_options::option_type::INTEGER,  "Size of the auto-generated last played playlist, 0 to disable" },
    { OPTION_LASTPLAYEDSKIPCOLLECTION, "",         global_options::option_type::MSTRING,  "Skip CSV list of collections being added to last played" },
    { OPTION_ACTION,                   "",         global_options::option_type::STRING,   "If action=<something> and the action has setting=<something> then perform animation" },
    { OPTION_ENTERONCOLLECTION,        "false",    global_options::option_type::BOOLEAN,  "Enter the collection when using collection up/down controls" },
    { OPTION_BACKONCOLLECTION,         "false",    global_options::option_type::BOOLEAN,  "Move to the next/previous collection when using the collectionUp/Down/Left/Right buttons" },
    { OPTION_STARTCOLLECTIONENTER,     "false",    global_options::option_type::BOOLEAN,  "Enter the first collection on RetroFE boot" },
    { OPTION_EXITONFIRSTPAGEBACK,      "false",    global_options::option_type::BOOLEAN,  "Exit RetroFE when the back button is pressed on the first page" },
    { OPTION_REMEMBERMENU,             "true",     global_options::option_type::BOOLEAN,  "Remember the last highlighted item if re-entering a menu" },
    { OPTION_BACKONEMPTY,              "false",    global_options::option_type::BOOLEAN,  "Automatically back out of empty collection" },
    { OPTION_SUBSSPLIT,                "false",    global_options::option_type::BOOLEAN,  "Split merged collections based on subs (true) or sort as one list (false)" },
    { OPTION_CFWLETTERSUB,             "false",    global_options::option_type::BOOLEAN,  "Jump subs in a collection by sub instead of by the letter of the item" },
    { OPTION_PREVLETTERSUBTOCURRENT,   "false",    global_options::option_type::BOOLEAN,  "Jump to the start of the current letter instead of the previous letter if jump to letter enabled" },
    { OPTION_RANDOMSTART,              "false",    global_options::option_type::BOOLEAN,  "Start on a random item when RetroFE boots" },
    { OPTION_KIOSK,                    "false",    global_options::option_type::BOOLEAN,  "Start on the first playlist in cyclePlaylist with navigation and favorites locked, can be toggled with a setting in controls.conf" },
    { OPTION_GLOBALFAVLAST,            "false",    global_options::option_type::BOOLEAN,  "Save last played and favorites to a new collection" },
    { OPTION_INFOEXITONSCROLL,         "false",    global_options::option_type::BOOLEAN,  "Hide info text boxes when scrolling" },
    { OPTION_JUKEBOX,                  "false",    global_options::option_type::BOOLEAN,  "Enables mapping of jukebox controls" },
    { OPTION_FIXEDRESLAYOUTS,          "false",    global_options::option_type::BOOLEAN,  "Enables the use of fixed resolution layouts ie layout1920x1080.xml"},
    { OPTION_SCREENSAVER,              "false",    global_options::option_type::BOOLEAN,  "Enables screensaver mode"},

    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "ATTRACT MODE OPTIONS" },
    { OPTION_ATTRACTMODECYCLEPLAYLIST, "false",    global_options::option_type::BOOLEAN,  "Cycle through all playlists or defined in cyclePlaylist" },
    { OPTION_ATTRACTMODETIME,          "19",       global_options::option_type::INTEGER,  "Number of seconds to wait before scrolling to another random point" },
    { OPTION_ATTRACTMODENEXTTIME,      "19",       global_options::option_type::INTEGER,  "Number of seconds to wait before scrolling to another random point while attract mode is active" },
    { OPTION_ATTRACTMODEPLAYLISTTIME,  "300",      global_options::option_type::INTEGER,  "Number of seconds to wait before attract mode jumps to another playlist, 0 to lock" },
    { OPTION_ATTRACTMODESKIPPLAYLIST,   "",        global_options::option_type::MSTRING,  "Skip CSV list of playlists while in attract mode" },
    { OPTION_ATTRACTMODECOLLECTIONTIME, "300",     global_options::option_type::INTEGER,  "Number of seconds before attract mode switches to the next collection, 0 to lock" },
    { OPTION_ATTRACTMODESKIPCOLLECTION, "",        global_options::option_type::MSTRING,  "Skip CSV list of collections while in attract mode" },
    { OPTION_ATTRACTMODEMINTIME,        "100",     global_options::option_type::INTEGER,  "Minimum number of milliseconds attract mode will scroll" },
    { OPTION_ATTRACTMODEMAXTIME,        "1600",    global_options::option_type::INTEGER,  "Maximum number of milliseconds attract mode will scroll" },
    { OPTION_ATTRACTMODEFAST,           "false",   global_options::option_type::BOOLEAN,  "Scroll(false) or jump(true) to the next random point while in attract mode" },

    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "INPUT OPTIONS" },
    { OPTION_COLLECTIONINPUTCLEAR,     "false",    global_options::option_type::BOOLEAN,  "Clear input queue on collection change" },
    { OPTION_PLAYLISTINPUTCLEAR,       "false",    global_options::option_type::BOOLEAN,  "Clear input queue on playlist change" },
    { OPTION_JUMPINPUTCLEAR,           "false",    global_options::option_type::BOOLEAN,  "Clear input queue while jumping through the menu" },
    { OPTION_CONTROLLERCOMBOEXIT,      "true",     global_options::option_type::BOOLEAN,  "Close RetroFE with the controller combo set in controls.conf" },
    { OPTION_CONTROLLERCOMBOSETTINGS,  "false",    global_options::option_type::BOOLEAN,  "Open settings playlist with the controller combo set in controls.conf" },
    { OPTION_SETTINGSCOLLECTIONPLAYLIST,"Arcades:settings", global_options::option_type::STRING,   "Used by settings toggle to go to the playlist in collection:playlist format, defaults to settings.txt in the current collection" },

    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "METADATA OPTIONS" },
    { OPTION_METALOCK,                 "true",     global_options::option_type::BOOLEAN,  "Locks RetroFE from looking for XML changes and uses meta.db, faster loading when true" },
    { OPTION_OVERWRITEXML,             "false",    global_options::option_type::BOOLEAN,  "Allows metadata XMLs to be overwritten by files in a collection" },
    { OPTION_SHOWPARENTHESIS,          "true",     global_options::option_type::BOOLEAN,  "Show item information between ()" },
    { OPTION_SHOWSQUAREBRACKETS,       "true",     global_options::option_type::BOOLEAN,  "Show item information between []" },

    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "WINDOWS ONLY OPTIONS" },
    { OPTION_LEDBLINKYDIRECTORY,       "",         global_options::option_type::PATH,     "Path to LEDBlinky installation" },
    { OPTION_LEDBLINKYCLOSEONEXIT,     "true",     global_options::option_type::BOOLEAN,  "If set to false, LEDBlinky will not close with RetroFE and keep the session open" },

    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "MEDIA SEARCH PATH OPTIONS" },
    { OPTION_BASEMEDIAPATH,            "",         global_options::option_type::PATH,     "Path to media if stored outside the build" },
    { OPTION_BASEITEMPATH,             "",         global_options::option_type::PATH,     "Path to items if stored outside the build" },

    { nullptr }
};

// Function to format and print contents of global_options::options_entry
void showUsage(const global_options::options_entry* options) {
    for (int i = 0; options[i].name || options[i].description; ++i) {
        if (options[i].name) {
            std::cout << "-" << std::setw(30) << std::left << options[i].name << options[i].description << std::endl;
        }
        else {
            // Category headers have nullptr names, so we print separate
            std::cout << "\n#\n# " << options[i].description << "\n#\n" << std::endl;
        }
    }
    std::cout << std::endl;
}

// Function to format and print contents of global_options::options_entry to a settings file
void makeSettings(const global_options::options_entry* options) {
    std::string filename = Utils::combinePath(Configuration::absolutePath, "settings - default.conf");
    std::ofstream settingsFile;
    settingsFile.open(filename.c_str());
    for (int i = 0; options[i].name || options[i].description; ++i) {
        if (options[i].name) {
            settingsFile << options[i].name << "=" << options[i].defvalue << std::endl;
        }
        else {
            // Category headers have nullptr names, so we print separate
            settingsFile << "\n# " << options[i].description << "\n" << std::endl;
        }
    }
    settingsFile.close();
}

// Function to format and print contents of global_options::options_entry to a settings file
void makeSettingsReadme(const global_options::options_entry* options) {
    std::string filename = Utils::combinePath(Configuration::absolutePath, "settings - README.txt");
    std::ofstream settingsFile;
    settingsFile.open(filename.c_str());
    for (int i = 0; options[i].name || options[i].description; ++i) {
        if (options[i].name) {
            settingsFile << std::setw(30) << std::left << options[i].name << options[i].description << std::endl;
        }
        else {
            // Category headers have nullptr names, so we print separate
            settingsFile << "\n#\n# " << options[i].description << "\n#\n" << std::endl;
        }
    }
    settingsFile.close();
}
