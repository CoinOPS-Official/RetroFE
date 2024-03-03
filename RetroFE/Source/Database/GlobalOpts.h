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

// INPUT OPTIONS
#define OPTION_COLLECTIONINPUTCLEAR    "collectionInputClear"
#define OPTION_PLAYLISTINPUTCLEAR      "playlistInputClear"
#define OPTION_JUMPINPUTCLEAR          "jumpInputClear"
#define OPTION_CONTROLLERCOMBOEXIT     "controllerComboExit"
#define OPTION_CONTROLLERCOMBOSETTINGS "controllerComboSettings"
#define OPTION_SETTINGSCOLLECTIONPLAYLIST "settingsCollectionPlaylist"

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
        const char *                name;               // name on the command line
        const char *                defvalue;           // default value of this argument
        option_type                 type;               // type of option
        const char *                description;        // description for -showusage
    };
    
    // TODO implement writing

//    // reading
    const char *value(std::string_view defvalue) const noexcept;
    const char *description(std::string_view defvalue) const noexcept;
    bool bool_value(std::string_view defvalue) const { return int_value(defvalue) != 0; }
    int int_value(std::string_view defvalue) const;
    float float_value(std::string_view defvalue) const;
    
    const char *log() const { return value(OPTION_LOG); }
    bool dumpproperties() const { return bool_value(OPTION_DUMPPROPERTIES); }
    
    int numscreens() const { return int_value(OPTION_NUMSCREENS); }
    bool fullscreen() const { return bool_value(OPTION_FULLSCREEN); }
    int horizontal() const { return int_value(OPTION_HORIZONTAL); }
    int vertical() const { return int_value(OPTION_VERTICAL); }
    int screennum() const { return int_value(OPTION_SCREENNUM); }
    bool fullscreenx() const { return bool_value(OPTION_FULLSCREENX); }
    int horizontalx() const { return int_value(OPTION_HORIZONTALX); }
    int verticalx() const { return int_value(OPTION_VERTICALX); }
    int screennumx() const { return int_value(OPTION_SCREENNUMX); }
    bool mirrorx() const { return bool_value(OPTION_MIRRORX); }
    int rotationx() const { return int_value(OPTION_ROTATIONX); }
    
    bool windowborder() const { return bool_value(OPTION_WINDOWBORDER); }
    bool windowresize() const { return bool_value(OPTION_WINDOWRESIZE); }
    int fps() const { return int_value(OPTION_FPS); }
    int fpsidle() const { return int_value(OPTION_FPSIDLE); }
    bool hidemouse() const { return bool_value(OPTION_HIDEMOUSE); }
    bool animateduringgame() const { return bool_value(OPTION_ANIMATEDURINGGAME); }
    
    bool videoenable() const { return bool_value(OPTION_VIDEOENABLE); }
    int videoloop() const { return int_value(OPTION_VIDEOLOOP); }
    bool disablevideorestart() const { return bool_value(OPTION_DISABLEVIDEORESTART); }
    bool disablepauseonscroll() const { return bool_value(OPTION_DISABLEPAUSEONSCROLL); }
    
    bool vsync() const { return bool_value(OPTION_VSYNC); }
    bool hardwarevideoaccel() const { return bool_value(OPTION_HARDWAREVIDEOACCEL); }
    int avdecmaxthreads() const { return int_value(OPTION_AVDECMAXTHREADS); }
    bool mutevideo() const { return bool_value(OPTION_MUTEVIDEO); }
    int sdlrenderdriver() const { return int_value(OPTION_SDLRENDERDRIVER); }
    int scalequality() const { return int_value(OPTION_SCALEQUALITY); }
    bool highpriority() const { return bool_value(OPTION_HIGHPRIORITY); }
    bool unloadsdl() const { return bool_value(OPTION_UNLOADSDL); }
    bool minimizeonfocusloss() const { return bool_value(OPTION_MINIMIZEONFOCUSLOSS); }
    int avdecthreadtype() const {return int_value(OPTION_AVDECTHREADTYPE); }
    int glswapinterval() const { return int_value(OPTION_GLSWAPINTERVAL); }
    
    const char *layout() const { return value(OPTION_LAYOUT); }
    const char *randomlayout() const { return value(OPTION_RANDOMLAYOUT); }
    const char *firstplaylist() const { return value(OPTION_FIRSTPLAYLIST); }
    const char *autoplaylist() const { return value(OPTION_AUTOPLAYLIST); }
    const char *cycleplaylist() const { return value(OPTION_CYCLEPLAYLIST); }
    const char *firstcollection() const { return value(OPTION_FIRSTCOLLECTION); }
    const char *cyclecollection() const { return value(OPTION_CYCLECOLLECTION); }
    int lastplayedsize() const { return int_value(OPTION_LASTPLAYEDSIZE); }
    const char *lastplayedskipcollection() const { return value(OPTION_LASTPLAYEDSKIPCOLLECTION); }
    bool enteroncollection() const { return bool_value(OPTION_ENTERONCOLLECTION); }
    bool backoncollection() const { return bool_value(OPTION_BACKONCOLLECTION); }
    bool startcollectionenter() const { return bool_value(OPTION_STARTCOLLECTIONENTER); }
    bool exitonfirstpageback() const { return bool_value(OPTION_EXITONFIRSTPAGEBACK); }
    bool remembermenu() const { return bool_value(OPTION_REMEMBERMENU); }
    bool backonempty() const { return bool_value(OPTION_BACKONEMPTY); }
    bool subssplit() const { return bool_value(OPTION_SUBSSPLIT); }
    bool cfwlettersub() const { return bool_value(OPTION_CFWLETTERSUB); }
    bool prevlettersubtocurrent() const { return bool_value(OPTION_PREVLETTERSUBTOCURRENT); }
    bool randomstart() const { return bool_value(OPTION_RANDOMSTART); }
    bool kiosk() const { return bool_value(OPTION_KIOSK); }
    bool globalfavlast() const { return bool_value(OPTION_GLOBALFAVLAST); }
    bool infoexitonscroll() const { return bool_value(OPTION_INFOEXITONSCROLL); }
    bool jukebox() const { return bool_value(OPTION_JUKEBOX); }
    bool fixedreslayouts() const { return bool_value(OPTION_FIXEDRESLAYOUTS); }
    bool screensaver() const { return bool_value(OPTION_SCREENSAVER); }

    bool attractmodecycleplaylist() const { return bool_value(OPTION_ATTRACTMODECYCLEPLAYLIST); }
    int attractmodetime() const { return int_value(OPTION_ATTRACTMODETIME); }
    int attractmodenexttime() const { return int_value(OPTION_ATTRACTMODENEXTTIME); }
    int attractmodeplaylisttime() const { return int_value(OPTION_ATTRACTMODEPLAYLISTTIME); }
    const char *attractmodeskipplaylist() const { return value(OPTION_ATTRACTMODESKIPPLAYLIST); }
    int attractmodecollectiontime() const { return int_value(OPTION_ATTRACTMODECOLLECTIONTIME); }
    const char *attractmodeskipcollection() const { return value(OPTION_ATTRACTMODESKIPCOLLECTION); }
    int attractmodemintime() const { return int_value(OPTION_ATTRACTMODEMINTIME); }
    int attractmodemaxtime() const { return int_value(OPTION_ATTRACTMODEMAXTIME); }
    bool attractmodefast() const { return bool_value(OPTION_ATTRACTMODEFAST); }
    
    bool collectioninputclear() const { return bool_value(OPTION_COLLECTIONINPUTCLEAR); }
    bool playlistinputclear() const { return bool_value(OPTION_PLAYLISTINPUTCLEAR); }
    bool jumpinputclear() const { return bool_value(OPTION_JUMPINPUTCLEAR); }
    bool controllercomboexit() const { return bool_value(OPTION_CONTROLLERCOMBOEXIT); }
    bool controllercombosettings() const { return bool_value(OPTION_CONTROLLERCOMBOSETTINGS); }
    const char *settingscollectionplaylist() const { return value(OPTION_SETTINGSCOLLECTIONPLAYLIST); }
    
    bool metalock() const { return bool_value(OPTION_METALOCK); }
    bool overwritexml() const { return bool_value(OPTION_OVERWRITEXML); }
    bool showparenthesis() const { return bool_value(OPTION_SHOWPARENTHESIS); }
    bool showsquarebrackets() const { return bool_value(OPTION_SHOWSQUAREBRACKETS); }
    
    const char *ledblinkydirectory() const { return value(OPTION_LEDBLINKYDIRECTORY); }
    
    const char *basemediapath() const { return value(OPTION_BASEMEDIAPATH); }
    const char *baseitempath() const { return value(OPTION_BASEITEMPATH); }
    
    // static list of options entries
    static const options_entry s_option_entries[];
};

// Function to format and print contents of global_options::options_entry
void showUsage(const global_options::options_entry* options);

// Function to format and print contents of global_options::options_entry to a settings file
void makeSettings(const global_options::options_entry* options);

// Function to format and print contents of global_options::options_entry to a readme file
void makeSettingsReadme(const global_options::options_entry* options);
