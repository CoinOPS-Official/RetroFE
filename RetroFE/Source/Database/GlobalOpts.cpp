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

// Function to update the SDL Render Driver based on platform
const char * setSDL() {
    #ifdef WIN32
        {return "direct3d11";}
    #elif __APPLE__
        { return "metal";}
    #else
        {return "opengl";}
    #endif
}

const global_options::options_entry global_options::s_option_entries[] =
{
    
    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "SYSTEM AND INPUT OPTIONS", "" },
    { OPTION_LOG,                     "NONE",      global_options::option_type::STRING,   "Set logging level, any combo of ERROR,INFO,NOTICE,WARNING,DEBUG,FILECACHE or ALL or NONE", "Logging Level" },
    { OPTION_DUMPPROPERTIES,          "false",     global_options::option_type::BOOLEAN,  "Dump contents of properties to a txt file in current directory", "Dump Properties"},

    { OPTION_COLLECTIONINPUTCLEAR,     "false",    global_options::option_type::BOOLEAN,  "Clear input queue on collection change", "Clear Input Queue on Collection Change" },
    { OPTION_PLAYLISTINPUTCLEAR,       "false",    global_options::option_type::BOOLEAN,  "Clear input queue on playlist change", "Clear Input Queue on Playlist Change" },
    { OPTION_JUMPINPUTCLEAR,           "false",    global_options::option_type::BOOLEAN,  "Clear input queue while jumping through the menu", "Clear Input Queue While Jumping Through Menu" },
    { OPTION_CONTROLLERCOMBOEXIT,      "true",     global_options::option_type::BOOLEAN,  "Close RetroFE with the controller combo set in controls.conf", "Enable Controller Combo Exit" },
    { OPTION_CONTROLLERCOMBOSETTINGS,  "false",    global_options::option_type::BOOLEAN,  "Open settings playlist with the controller combo set in controls.conf", "Enable Opening Settings with Controller" },
    { OPTION_SETTINGSCOLLECTIONPLAYLIST,"Arcades:settings", global_options::option_type::STRING,   "Used by settings toggle to go to the playlist in collection:playlist format, defaults to settings.txt in the current collection", "Settings Collection Playlist" },



    { nullptr,                        nullptr,     global_options::option_type::HEADER,   "DISPLAY AND VIDEO OPTIONS", "" },
    { OPTION_NUMSCREENS,              "1",         global_options::option_type::INTEGER,  "Defines the number of monitors used", "Number of Screens" },
    { OPTION_FULLSCREEN,              "true",      global_options::option_type::BOOLEAN,  "Run the frontend in fullscreen", "Enable Fullscreen" },
    { OPTION_HORIZONTAL,              "stretch",   global_options::option_type::STRING,   "Pixel width integer or stretch", "Horizontal Resolution" },
    { OPTION_VERTICAL,                "stretch",   global_options::option_type::STRING,   "Pixel height integer or stretch", "Vertical Resolution" },
    { OPTION_FULLSCREENX,             "true",      global_options::option_type::BOOLEAN,  "Run the frontend in fullscreen for monitor x", "Fullscreen Monitor X" },
    { OPTION_HORIZONTALX,             "",          global_options::option_type::INTEGER,  "Pixel width for monitor x", "Horizontal Monitor X" },
    { OPTION_VERTICALX,               "",          global_options::option_type::INTEGER,  "Pixel height for monitor x", "Vertical Monitor X" },
    { OPTION_SCREENNUMX,              "",          global_options::option_type::INTEGER,  "Define which monitor x is which display window, Screen numbers start at 0!", "Monitor Screen Number" },
    { OPTION_MIRRORX,                 "false",     global_options::option_type::BOOLEAN,  "Divides monitor x into two halves", "Mirror Monitor X" },
    { OPTION_ROTATIONX,               "0",         global_options::option_type::INTEGER,  "Rotation of monitor x (0, 1, 2, 3)", "Rotation Monitor X" },

    { OPTION_WINDOWBORDER,            "false",     global_options::option_type::BOOLEAN,  "Show window decorations", "Enable Window Border" },
    { OPTION_WINDOWRESIZE,            "false",     global_options::option_type::BOOLEAN,  "Allow RetroFE to be resized", "Enable Window Resizing" },
    { OPTION_FPS,                     "60",        global_options::option_type::INTEGER,  "Requested FPS while in an active state", "Target FPS" },
    { OPTION_FPSIDLE,                 "60",        global_options::option_type::INTEGER,  "Request FPS while in an idle state", "Idle FPS" },
    { OPTION_HIDEMOUSE,               "true",      global_options::option_type::BOOLEAN,  "Hide the mouse cursor when RetroFE is active", "Hide Mouse Cursor" },
    { OPTION_ANIMATEDURINGGAME,       "true",      global_options::option_type::BOOLEAN,  "Play animated marquees while in game", "Enable Animations During Game" },

    { OPTION_VIDEOENABLE,             "true",      global_options::option_type::BOOLEAN,  "Defines whether video is rendered", "Enable Video Support" },
    { OPTION_VIDEOLOOP,               "0",         global_options::option_type::INTEGER,  "Number of times to play video<br><br>If unsure leave at 0", "Video Loop Count" },
    { OPTION_DISABLEVIDEORESTART,     "false",     global_options::option_type::BOOLEAN,  "Pauses video while scrolling for performance increase.<br><br> If unsure leave unchecked", "Enable Pausing Video on Scroll" },
    { OPTION_DISABLEPAUSEONSCROLL,    "false",     global_options::option_type::BOOLEAN,  "Disables restarting video when selected.<br><br> If unsure leave unchecked", "Enable Restart Video on Selection" },

    { OPTION_VSYNC,                    "false",    global_options::option_type::BOOLEAN,  "Prevents screen tearing by synchronizing the <br>frame rate with the monitor's refresh rate, ensuring smoother visuals.", "Enable V-Sync" },
    { OPTION_HARDWAREVIDEOACCEL,       "false",    global_options::option_type::BOOLEAN,  "Enhances video playback performance by offloading decoding tasks to the GPU,<br> reducing CPU usage and improving playback smoothness.", "Enable Hardware Video Acceleration" },
    { OPTION_AVDECMAXTHREADS,          "2",        global_options::option_type::INTEGER,  "Specifies the number of threads used by the audio/video decoder,<br> allowing for parallel processing to improve performance and reduce playback stuttering.", "AV Decoder Threads" },
    { OPTION_MUTEVIDEO,                "false",    global_options::option_type::BOOLEAN,  "Video playback is muted", "Mute Video Playback" },
    { OPTION_SDLRENDERDRIVER,          setSDL(),   global_options::option_type::STRING,   "Selects which graphics API to use internally.<br><br>The software renderer is extremely slow and only useful for debugging, so any of the other backends are recommended.", "SDL Render Driver" },
    { OPTION_SCALEQUALITY,             "1",        global_options::option_type::INTEGER,  "Scaling quality (0, 1, 2)", "Scale Quality" },
    { OPTION_HIGHPRIORITY,             "false",    global_options::option_type::BOOLEAN,  "Adjusts the priority level of the RetroFE, affecting its CPU time allocation.", "Enable High Priority Process" },
    { OPTION_UNLOADSDL,                "false",    global_options::option_type::BOOLEAN,  "Close SDL when launching a game, MUST be true for RPI", "Enable Unload SDL on Game Launch" },
    { OPTION_MINIMIZEONFOCUSLOSS,      "false",    global_options::option_type::BOOLEAN,  "Minimize RetroFE when focus is lost", "Enable Minimize on Focus Loss" },
    { OPTION_AVDECTHREADTYPE,          "2",        global_options::option_type::INTEGER,  "Type of threading in the case of software decoding (1=frame, 2=slice)", "AV Decoder Thread Type" },



    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "MEDIA AND METADATA OPTIONS", "" },
    { OPTION_ATTRACTMODECYCLEPLAYLIST, "false",    global_options::option_type::BOOLEAN,  "Cycle through all playlists or defined in cyclePlaylist", "Enable Cycling Through All Playlists in Attract Mode" },
    { OPTION_ATTRACTMODETIME,          "19",       global_options::option_type::INTEGER,  "Number of seconds to wait before scrolling to another random point", "Attract Mode Wait Time" },
    { OPTION_ATTRACTMODENEXTTIME,      "19",       global_options::option_type::INTEGER,  "Number of seconds to wait before scrolling to another random point while attract mode is active", "Attract Mode Next Wait Time" },
    { OPTION_ATTRACTMODEPLAYLISTTIME,  "300",      global_options::option_type::INTEGER,  "Number of seconds to wait before attract mode jumps to another playlist, 0 to lock", "Attract Mode Playlist Switch Time" },
    { OPTION_ATTRACTMODESKIPPLAYLIST,  "",         global_options::option_type::MSTRING,  "Skip CSV list of playlists while in attract mode", "Skip Playlists in Attract Mode" },
    { OPTION_ATTRACTMODECOLLECTIONTIME, "300",     global_options::option_type::INTEGER,  "Number of seconds before attract mode switches to the next collection, 0 to lock", "Attract Mode Collection Switch Time" },
    { OPTION_ATTRACTMODESKIPCOLLECTION, "",        global_options::option_type::MSTRING,  "Skip CSV list of collections while in attract mode", "Skip Collections in Attract Mode" },
    { OPTION_ATTRACTMODEMINTIME,        "100",     global_options::option_type::INTEGER,  "Minimum number of milliseconds attract mode will scroll", "Minimum Attract Mode Scroll Time" },
    { OPTION_ATTRACTMODEMAXTIME,        "1600",    global_options::option_type::INTEGER,  "Maximum number of milliseconds attract mode will scroll", "Maximum Attract Mode Scroll Time" },
    { OPTION_ATTRACTMODEFAST,           "false",   global_options::option_type::BOOLEAN,  "Scroll(false) or jump(true) to the next random point while in attract mode", "Enable Fast Scroll in Attract Mode" },

    { OPTION_METALOCK,                 "true",     global_options::option_type::BOOLEAN,  "Locks RetroFE from looking for XML changes and uses meta.db.<br><br> If unsure leave checked", "Enable Metadata Lock" },
    { OPTION_OVERWRITEXML,             "false",    global_options::option_type::BOOLEAN,  "Allows metadata XMLs to be overwritten by files in a collection.<br><br> If unsure leave unchecked", "Allow XML Overwrite by Collection Files" },
    { OPTION_SHOWPARENTHESIS,          "true",     global_options::option_type::BOOLEAN,  "Show item information between ().<br><br> If unsure leave checked", "Enable Show Item Info Between Parentheses" },
    { OPTION_SHOWSQUAREBRACKETS,       "true",     global_options::option_type::BOOLEAN,  "Show item information between []<br><br> If unsure leave checked", "Enable Show Item Info Between Square Brackets" },



    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "CUSTOMIZATION OPTIONS", "" },
    { OPTION_LAYOUT,                   "Arcades",  global_options::option_type::STRING,   "Theme to be used in RetroFE, a folder name in /layouts", "RetroFE Theme" },
    { OPTION_RANDOMLAYOUT,             "",         global_options::option_type::MSTRING,  "Randomly choose a layout on launch, CSV list of layout names", "Random Layouts" },
    { OPTION_FIRSTPLAYLIST,            "arcades",  global_options::option_type::STRING,   "Start on this playlist if available", "First Playlist" },
    { OPTION_AUTOPLAYLIST,             "all",      global_options::option_type::STRING,   "Start on this playlist when entering a collection if available", "Auto Playlist" },
    { OPTION_CYCLEPLAYLIST,            "",         global_options::option_type::MSTRING,  "Set of playlists that can be cycled through, CSV list of playlist names", "Cycle Playlists" },
    { OPTION_FIRSTCOLLECTION,          "",         global_options::option_type::STRING,   "Start on this collection if available", "First Collection" },
    { OPTION_CYCLECOLLECTION,          "",         global_options::option_type::MSTRING,  "Set of collections that can be cycled through, CSV list of collection names", "Cycle Collections" },
    { OPTION_LASTPLAYEDSIZE,           "10",       global_options::option_type::INTEGER,  "Size of the auto-generated last played playlist, 0 to disable", "Last Played Playlist Size" },
    { OPTION_LASTPLAYEDSKIPCOLLECTION, "",         global_options::option_type::MSTRING,  "Skip CSV list of collections being added to last played", "Skip Collections for Last Played" },
    { OPTION_ACTION,                   "",         global_options::option_type::STRING,   "If action=<something> and the action has setting=<something> then perform animation", "Action Animation" },
    { OPTION_ENTERONCOLLECTION,        "false",    global_options::option_type::BOOLEAN,  "Enter the collection when using collection up/down controls", "Enter on Collection" },
    { OPTION_BACKONCOLLECTION,         "false",    global_options::option_type::BOOLEAN,  "Move to the next/previous collection when using the collectionUp/Down/Left/Right buttons", "Back on Collection" },
    { OPTION_STARTCOLLECTIONENTER,     "false",    global_options::option_type::BOOLEAN,  "Enter the first collection on RetroFE boot", "Enter the First Collection At Boot" },
    { OPTION_EXITONFIRSTPAGEBACK,      "false",    global_options::option_type::BOOLEAN,  "Exit RetroFE when the back button is pressed on the first page", "Exit on First Page Back" },
    { OPTION_REMEMBERMENU,             "true",     global_options::option_type::BOOLEAN,  "Remember the last highlighted item if re-entering a menu", "Remember Menu" },
    { OPTION_BACKONEMPTY,              "false",    global_options::option_type::BOOLEAN,  "Automatically back out of empty collection", "Back on Empty Collection" },
    { OPTION_SUBSSPLIT,                "false",    global_options::option_type::BOOLEAN,  "Split merged collections based on subs (true) or sort as one list (false)", "Enable Split Subs" },
    { OPTION_CFWLETTERSUB,             "false",    global_options::option_type::BOOLEAN,  "Jump subs in a collection by sub instead of by the letter of the item", "Enable CFW Letter Sub" },
    { OPTION_PREVLETTERSUBTOCURRENT,   "false",    global_options::option_type::BOOLEAN,  "Jump to the start of the current letter instead of the previous letter if jump to letter enabled", "Previous Letter Sub to Current" },
    { OPTION_RANDOMSTART,              "false",    global_options::option_type::BOOLEAN,  "Start on a random item when RetroFE boots", "Enable Random Start Item" },
    { OPTION_KIOSK,                    "false",    global_options::option_type::BOOLEAN,  "Start on the first playlist in cyclePlaylist with navigation and favorites locked, can be toggled with a setting in controls.conf", "Enable Kiosk Mode" },
    { OPTION_GLOBALFAVLAST,            "false",    global_options::option_type::BOOLEAN,  "Save last played and favorites to a new collection", "Global Favorites Last" },
    { OPTION_INFOEXITONSCROLL,         "false",    global_options::option_type::BOOLEAN,  "Hide info text boxes when scrolling", "Info Exit on Scroll" },
    { OPTION_JUKEBOX,                  "false",    global_options::option_type::BOOLEAN,  "Enables mapping of jukebox controls", "Enable Jukebox Mode" },
    { OPTION_FIXEDRESLAYOUTS,          "false",    global_options::option_type::BOOLEAN,  "Enables the use of fixed resolution layouts ie layout1920x1080.xml", "Enable Fixed Resolution Layouts" },
    { OPTION_SCREENSAVER,              "false",    global_options::option_type::BOOLEAN,  "Enables screensaver mode", "Enable Screensaver" },



    { nullptr,                         nullptr,    global_options::option_type::HEADER,   "LEDBlinky and Path Overrides", "" },
    { OPTION_LEDBLINKYDIRECTORY,       "",         global_options::option_type::PATH,     "Path to LEDBlinky.exe", "LEDBlinky Install Path" },

    { OPTION_BASEMEDIAPATH,            "",         global_options::option_type::PATH,     "Override path to media if stored outside directory", "Media Path Override" },
    { OPTION_BASEITEMPATH,             "",         global_options::option_type::PATH,     "Override path to items if stored outside directory", "Item Path Overrides" },

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

// // Function to format and print contents of global_options::options_entry to a settings file
// void makeSettings(const global_options::options_entry* options) {
//     std::string filename = Utils::combinePath(Configuration::absolutePath, "settings - default.conf");
//     std::ofstream settingsFile;
//     settingsFile.open(filename.c_str());
//     for (int i = 0; options[i].name || options[i].description; ++i) {
//         if (options[i].name) {
//             settingsFile << options[i].name << "=" << options[i].defvalue << std::endl;
//         }
//         else {
//             // Category headers have nullptr names, so we print separate
//             settingsFile << "\n# " << options[i].description << "\n" << std::endl;
//         }
//     }
//     settingsFile.close();
// }

// // Function to format and print contents of global_options::options_entry to a settings file
// void makeSettingsReadme(const global_options::options_entry* options) {
//     std::string filename = Utils::combinePath(Configuration::absolutePath, "settings - README.txt");
//     std::ofstream settingsFile;
//     settingsFile.open(filename.c_str());
//     for (int i = 0; options[i].name || options[i].description; ++i) {
//         if (options[i].name) {
//             settingsFile << std::setw(30) << std::left << options[i].name << options[i].description << std::endl;
//         }
//         else {
//             // Category headers have nullptr names, so we print separate
//             settingsFile << "\n#\n# " << options[i].description << "\n#\n" << std::endl;
//         }
//     }
//     settingsFile.close();
// }
