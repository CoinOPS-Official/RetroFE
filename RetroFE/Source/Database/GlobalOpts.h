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

#include <string>

//**************************************************************************
//  Options file and command line management
//**************************************************************************

// LOGGING OPTIONS
#define OPTION_LOG                   "log"
#define OPTION_DUMPPROPERTIES        "dumpProperties"

// DISPLAY OPTIONS
#define OPTION_NUMSCREENS            "numScreens"
#define OPTION_FULLSCREEN            "fullscreen"
#define OPTION_HORIZONTAL            "horizontal"
#define OPTION_VERTICAL              "vertical"
#define OPTION_SCREENNUM             "screenNum"
#define OPTION_MIRROR                "mirror"
#define OPTION_ROTATION              "rotation"
#define OPTION_FULLSCREENX           "fullscreenx"
#define OPTION_HORIZONTALX           "horizontalx"
#define OPTION_VERTICALX             "verticalx"
#define OPTION_SCREENNUMX            "screenNumx"
#define OPTION_MIRRORX               "mirrorx"
#define OPTION_ROTATIONX             "rotationx"

// WINDOW OPTIONS
#define OPTION_WINDOWBORDER          "windowBorder"
#define OPTION_WINDOWRESIZE          "windowResize"
#define OPTION_FPS                   "fps"
#define OPTION_FPSIDLE               "fpsIdle"
#define OPTION_HIDEMOUSE             "hideMouse"
#define OPTION_ANIMATEDURINGGAME     "animateDuringGame"

// VIDEO OPTIONS
#define OPTION_VIDEOENABLE           "videoEnable"
#define OPTION_VIDEOLOOP             "videoLoop"
#define OPTION_DISABLEVIDEORESTART   "disableVideoRestart"
#define OPTION_DISABLEPAUSEONSCROLL  "disablePauseOnScroll"

// RENDERER OPTIONS
#define OPTION_VSYNC                 "vSync"
#define OPTION_HARDWAREVIDEOACCEL    "HardwareVideoAccel"
#define OPTION_AVDECMAXTHREADS       "AvdecMaxThreads"
#define OPTION_MUTEVIDEO             "MuteVideo"
#define OPTION_SDLRENDERDRIVER       "SDLRenderDriver"
#define OPTION_SCALEQUALITY          "ScaleQuality"
#define OPTION_HIGHPRIORITY          "highPriority"
#define OPTION_UNLOADSDL             "unloadSDL"
#define OPTION_MINIMIZEONFOCUSLOSS   "minimizeOnFocusLoss"
#define OPTION_AVDECTHREADTYPE       "AvdecThreadType"
#define OPTION_GLSWAPINTERVAL        "GlSwapInterval"

// CUSTOMIZATION OPTIONS
#define OPTION_LAYOUT                "layout"
#define OPTION_RANDOMLAYOUT          "randomLayout"
#define OPTION_FIRSTPLAYLIST         "firstPlaylist"
#define OPTION_QUICKLISTCOLLECTIONPLAYLIST             "quickListCollectionPlaylist"
#define OPTION_AUTOPLAYLIST          "autoPlaylist"
#define OPTION_CYCLEPLAYLIST         "cyclePlaylist"
#define OPTION_FIRSTCOLLECTION       "firstCollection"
#define OPTION_CYCLECOLLECTION       "cycleCollection"
#define OPTION_LASTPLAYEDSIZE         "lastplayedSize"
#define OPTION_LASTPLAYEDSKIPCOLLECTION "lastPlayedSkipCollection"
#define OPTION_ACTION                 "action"
#define OPTION_ENTERONCOLLECTION      "enterOnCollection"
#define OPTION_BACKONCOLLECTION       "backOnCollection"
#define OPTION_STARTCOLLECTIONENTER   "startCollectionEnter"
#define OPTION_EXITONFIRSTPAGEBACK    "exitOnFirstPageBack"
#define OPTION_REMEMBERMENU           "rememberMenu"
#define OPTION_BACKONEMPTY            "backOnEmpty"
#define OPTION_SUBSSPLIT              "subsSplit"
#define OPTION_CFWLETTERSUB           "cfwLetterSub"
#define OPTION_PREVLETTERSUBTOCURRENT  "prevLetterSubToCurrent"
#define OPTION_RANDOMSTART            "randomStart"
#define OPTION_RANDOMPLAYLIST         "randomPlaylist"
#define OPTION_KIOSK                  "kiosk"
#define OPTION_GLOBALFAVLAST          "globalFavLast"
#define OPTION_INFOEXITONSCROLL       "infoExitOnScroll"
#define OPTION_JUKEBOX                "jukebox"
#define OPTION_FIXEDRESLAYOUTS        "fixedResLayouts"
#define OPTION_SCREENSAVER            "screensaver"

// ATTRACT MODE OPTIONS
#define OPTION_ATTRACTMODECYCLEPLAYLIST "attractModeCyclePlaylist"
#define OPTION_ATTRACTMODETIME           "attractModeTime"
#define OPTION_ATTRACTMODENEXTTIME       "attractModeNextTime"
#define OPTION_ATTRACTMODEPLAYLISTTIME   "attractModePlaylistTime"
#define OPTION_ATTRACTMODESKIPPLAYLIST   "attractModeSkipPlaylist"
#define OPTION_ATTRACTMODECOLLECTIONTIME "attractModeCollectionTime"
#define OPTION_ATTRACTMODESKIPCOLLECTION "attractModeSkipCollection"
#define OPTION_ATTRACTMODEMINTIME        "attractModeMinTime"
#define OPTION_ATTRACTMODEMAXTIME        "attractModeMaxTime"
#define OPTION_ATTRACTMODEFAST           "attractModeFast"
#define OPTION_ATTRACTMODELAUNCH         "attractModeLaunch"
#define OPTION_ATTRACTMODELAUNCHRUNTIME  "attractModeLaunchRunTime"
#define OPTION_ATTRACTMODELAUNCHMINMAXSCROLLS   "attractModeLaunchMinMaxScrolls"

// INPUT OPTIONS
#define OPTION_COLLECTIONINPUTCLEAR    "collectionInputClear"
#define OPTION_PLAYLISTINPUTCLEAR      "playlistInputClear"
#define OPTION_JUMPINPUTCLEAR          "jumpInputClear"
#define OPTION_CONTROLLERCOMBOEXIT     "controllerComboExit"
#define OPTION_CONTROLLERCOMBOSETTINGS "controllerComboSettings"
#define OPTION_SETTINGSCOLLECTIONPLAYLIST "settingsCollectionPlaylist"
#define OPTION_SERVOSTIKENABLED "servoStikEnabled"

// METADATA OPTIONS
#define OPTION_METALOCK               "metaLock"
#define OPTION_OVERWRITEXML           "overwriteXML"
#define OPTION_SHOWPARENTHESIS        "showParenthesis"
#define OPTION_SHOWSQUAREBRACKETS     "showSquareBrackets"

// WINDOWS ONLY OPTIONS
#define OPTION_LEDBLINKYDIRECTORY     "LEDBlinkyDirectory"

// MEDIA SEARCH PATH OPTIONS
#define OPTION_BASEMEDIAPATH          "baseMediaPath"
#define OPTION_BASEITEMPATH           "baseItemPath"

// Forward declaration
struct options_entry;

class global_options
{
public:
    enum class option_type {
        INVALID,         // invalid
        HEADER,          // a header item
        COMMAND,         // a command
        BOOLEAN,         // boolean option
        INTEGER,         // integer option
        FLOAT,           // floating-point option
        STRING,          // string option
        MSTRING,         // comma-delimited string option
        PATH,            // single path option
    };

    // Definition of options_entry describing a single option with its description and default value
    struct options_entry {
        const char* name;               // name on the command line
        const char* defvalue;           // default value of this argument
        option_type                 type;               // type of option
        const char* description;        // description for -showusage
    };

    // Definition of functions to directly return the values of specific options
    const char* log() { return value(OPTION_LOG); }
    bool dumpproperties() { return bool_value(OPTION_DUMPPROPERTIES); }

    int numscreens() { return int_value(OPTION_NUMSCREENS); }
    bool fullscreen() { return bool_value(OPTION_FULLSCREEN); }
    const char* horizontal() { return value(OPTION_HORIZONTAL); }
    const char* vertical() { return value(OPTION_VERTICAL); }
    int screennum() { return int_value(OPTION_SCREENNUM); }
    bool fullscreenx() { return bool_value(OPTION_FULLSCREENX); }
    int horizontalx() { return int_value(OPTION_HORIZONTALX); }
    int verticalx() { return int_value(OPTION_VERTICALX); }
    int screennumx() { return int_value(OPTION_SCREENNUMX); }
    bool mirrorx() { return bool_value(OPTION_MIRRORX); }
    int rotationx() { return int_value(OPTION_ROTATIONX); }

    bool windowborder() { return bool_value(OPTION_WINDOWBORDER); }
    bool windowresize() { return bool_value(OPTION_WINDOWRESIZE); }
    int fps() { return int_value(OPTION_FPS); }
    int fpsidle() { return int_value(OPTION_FPSIDLE); }
    bool hidemouse() { return bool_value(OPTION_HIDEMOUSE); }
    bool animateduringgame() { return bool_value(OPTION_ANIMATEDURINGGAME); }

    bool videoenable() { return bool_value(OPTION_VIDEOENABLE); }
    int videoloop() { return int_value(OPTION_VIDEOLOOP); }
    bool disablevideorestart() { return bool_value(OPTION_DISABLEVIDEORESTART); }
    bool disablepauseonscroll() { return bool_value(OPTION_DISABLEPAUSEONSCROLL); }

    bool vsync() { return bool_value(OPTION_VSYNC); }
    bool hardwarevideoaccel() { return bool_value(OPTION_HARDWAREVIDEOACCEL); }
    int avdecmaxthreads() { return int_value(OPTION_AVDECMAXTHREADS); }
    bool mutevideo() { return bool_value(OPTION_MUTEVIDEO); }
    int sdlrenderdriver() { return int_value(OPTION_SDLRENDERDRIVER); }
    int scalequality() { return int_value(OPTION_SCALEQUALITY); }
    bool highpriority() { return bool_value(OPTION_HIGHPRIORITY); }
    bool unloadsdl() { return bool_value(OPTION_UNLOADSDL); }
    bool minimizeonfocusloss() { return bool_value(OPTION_MINIMIZEONFOCUSLOSS); }
    int avdecthreadtype() { return int_value(OPTION_AVDECTHREADTYPE); }
    int glswapinterval() { return int_value(OPTION_GLSWAPINTERVAL); }

    const char* layout() { return value(OPTION_LAYOUT); }
    const char* randomlayout() { return value(OPTION_RANDOMLAYOUT); }
    const char* firstplaylist() { return value(OPTION_FIRSTPLAYLIST); }
    const char* autoplaylist() { return value(OPTION_AUTOPLAYLIST); }
    const char* cycleplaylist() { return value(OPTION_CYCLEPLAYLIST); }
    const char* firstcollection() { return value(OPTION_FIRSTCOLLECTION); }
    const char* cyclecollection() { return value(OPTION_CYCLECOLLECTION); }
    int lastplayedsize() { return int_value(OPTION_LASTPLAYEDSIZE); }
    const char* lastplayedskipcollection() { return value(OPTION_LASTPLAYEDSKIPCOLLECTION); }
    bool enteroncollection() { return bool_value(OPTION_ENTERONCOLLECTION); }
    bool backoncollection() { return bool_value(OPTION_BACKONCOLLECTION); }
    bool startcollectionenter() { return bool_value(OPTION_STARTCOLLECTIONENTER); }
    bool exitonfirstpageback() { return bool_value(OPTION_EXITONFIRSTPAGEBACK); }
    bool remembermenu() { return bool_value(OPTION_REMEMBERMENU); }
    bool backonempty() { return bool_value(OPTION_BACKONEMPTY); }
    bool subssplit() { return bool_value(OPTION_SUBSSPLIT); }
    bool cfwlettersub() { return bool_value(OPTION_CFWLETTERSUB); }
    bool prevlettersubtocurrent() { return bool_value(OPTION_PREVLETTERSUBTOCURRENT); }
    bool randomstart() { return bool_value(OPTION_RANDOMSTART); }
    bool randomplaylist() { return bool_value(OPTION_RANDOMPLAYLIST);}
    bool kiosk() { return bool_value(OPTION_KIOSK); }
    bool globalfavlast() { return bool_value(OPTION_GLOBALFAVLAST); }
    bool infoexitonscroll() { return bool_value(OPTION_INFOEXITONSCROLL); }
    bool jukebox() { return bool_value(OPTION_JUKEBOX); }
    bool fixedreslayouts() { return bool_value(OPTION_FIXEDRESLAYOUTS); }
    bool screensaver() { return bool_value(OPTION_SCREENSAVER); }

    bool attractmodecycleplaylist() { return bool_value(OPTION_ATTRACTMODECYCLEPLAYLIST); }
    int attractmodetime() { return int_value(OPTION_ATTRACTMODETIME); }
    int attractmodenexttime() { return int_value(OPTION_ATTRACTMODENEXTTIME); }
    int attractmodeplaylisttime() { return int_value(OPTION_ATTRACTMODEPLAYLISTTIME); }
    const char* attractmodeskipplaylist() { return value(OPTION_ATTRACTMODESKIPPLAYLIST); }
    int attractmodecollectiontime() { return int_value(OPTION_ATTRACTMODECOLLECTIONTIME); }
    const char* attractmodeskipcollection() { return value(OPTION_ATTRACTMODESKIPCOLLECTION); }
    int attractmodemintime() { return int_value(OPTION_ATTRACTMODEMINTIME); }
    int attractmodemaxtime() { return int_value(OPTION_ATTRACTMODEMAXTIME); }
    bool attractmodefast() { return bool_value(OPTION_ATTRACTMODEFAST); }
    bool attractmodelaunch() { return bool_value(OPTION_ATTRACTMODELAUNCH); }
    int attractmodelaunchruntime() { return int_value(OPTION_ATTRACTMODELAUNCHRUNTIME); }
    int attractmodelaunchminmaxScrolls() { return int_value(OPTION_ATTRACTMODELAUNCHMINMAXSCROLLS); }
    
    bool collectioninputclear() { return bool_value(OPTION_COLLECTIONINPUTCLEAR); }
    bool playlistinputclear() { return bool_value(OPTION_PLAYLISTINPUTCLEAR); }
    bool jumpinputclear() { return bool_value(OPTION_JUMPINPUTCLEAR); }
    bool controllercomboexit() { return bool_value(OPTION_CONTROLLERCOMBOEXIT); }
    bool controllercombosettings() { return bool_value(OPTION_CONTROLLERCOMBOSETTINGS); }
	const char* quicklistcollectionplaylist() { return value(OPTION_QUICKLISTCOLLECTIONPLAYLIST); }
    const char *settingscollectionplaylist() { return value(OPTION_SETTINGSCOLLECTIONPLAYLIST); }
    bool servostickenabled() { return bool_value(OPTION_SERVOSTIKENABLED); }
    
    bool metalock() { return bool_value(OPTION_METALOCK); }
    bool overwritexml() { return bool_value(OPTION_OVERWRITEXML); }
    bool showparenthesis() { return bool_value(OPTION_SHOWPARENTHESIS); }
    bool showsquarebrackets() { return bool_value(OPTION_SHOWSQUAREBRACKETS); }
    
    const char *ledblinkydirectory() { return value(OPTION_LEDBLINKYDIRECTORY); }
    
    const char *basemediapath() { return value(OPTION_BASEMEDIAPATH); }
    const char *baseitempath() { return value(OPTION_BASEITEMPATH); }
    
    // static list of options entries
    static const options_entry s_option_entries[];
    
private:
    static const char* value(const char* optionName) {
        for (int i = 0; s_option_entries[i].description; ++i) {
            if (s_option_entries[i].name == nullptr) {
                continue;
            }
            else if (std::string(s_option_entries[i].name) == optionName && s_option_entries[i].type == global_options::option_type::STRING) {
                return s_option_entries[i].defvalue; // Found the matching option
            }
            else {
                continue;
            }
        }
        return nullptr; // Option not found or not a string type
    }

    static bool bool_value(const char* optionName) {
        for (int i = 0; s_option_entries[i].description; ++i) {
            if (s_option_entries[i].name == nullptr) {
                continue;
            }
            else if (std::string(s_option_entries[i].name) == optionName && s_option_entries[i].type == global_options::option_type::BOOLEAN) {
                if (std::string(s_option_entries[i].defvalue) == "false") { return false; }
                else if (std::string(s_option_entries[i].defvalue) == "true") { return true; }
                else { break; }
            }
            else {
                continue;
            }
        }
        return false; // Option not found or not a boolean type
    }

    static int int_value(const char* optionName) {
        for (int i = 0; s_option_entries[i].description; ++i) {
            if (s_option_entries[i].name == nullptr) {
                continue;
            }
            else if (std::string(s_option_entries[i].name) == optionName && s_option_entries[i].type == global_options::option_type::INTEGER) {
                return std::atoi(s_option_entries[i].defvalue); // Found the matching option
            }
            else {
                continue;
            }
        }
        return 0; // Option not found or not an integer type
    }

    static double float_value(const char* optionName) {
        for (int i = 0; s_option_entries[i].description; ++i) {
            if (s_option_entries[i].name == nullptr) {
                continue;
            }
            else if (std::string(s_option_entries[i].name) == optionName && s_option_entries[i].type == global_options::option_type::FLOAT) {
                return std::stod(s_option_entries[i].defvalue); // Found the matching option
            }
            else {
                continue;
            }
        }
        return 0.0; // Option not found or not a float type
    }

    // TODO Define similar functions for MSTRING, PATH, and other option types...

};

// Function to format and print contents of global_options::options_entry
void showUsage(const global_options::options_entry* options);

// Function to format and print contents of global_options::options_entry to a settings file
void makeSettings(const global_options::options_entry* options);

// Function to format and print contents of global_options::options_entry to a readme file
void makeSettingsReadme(const global_options::options_entry* options);
