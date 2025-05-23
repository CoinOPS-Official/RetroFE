# Changelog
All notable changes to this project will be documented in this file.
 
The format is based on [Keep a Changelog](http://keepachangelog.com/)
This project uses [Calendar Versioning](http://calver.org/) with [Semantic Versioning](http://semver.org/) for minor fixes and features
in the format YYMM.X

## Contributors
[@monkofthefunk](https://github.com/monkofthefunk)
[@inigomontoya](https://github.com/inigomontoya)
[@aidenjbass](https://github.com/aidenjbass)
[@arghs15](https://github.com/arghs15)
[@bluestang2006](https://github.com/bluestang2006)

## [YYMM.X] - YYYY-MM-DD
### Added
### Changed
### Fixed

## [2504.2] - 2025-05-05
### Changed
- Delete the Configuration RetroFE tool, it isn't used at all, never made it past production and will only ever work on Windows. A better solution in the form of Qt in C++ is coming shortly [@aidenjbass](https://github.com/aidenjbass)

## [2504.1] - 2025-04-14
### Added
- RetroFE version appended with branch name if not on master [@aidenjbass](https://github.com/aidenjbass)

## [2504.0] - 2025-04-13

### Added
- Window to display if splash.xml is invalid [@aidenjbass](https://github.com/aidenjbass)
- Subtractive logging values, ie ALL,-INFO is all logging but INFO [@aidenjbass](https://github.com/aidenjbass)
- Playlist menu scroll and select using up/down for Hor and left/right for Vert [@monkofthefunk](https://github.com/monkofthefunk)
- `CMake/Versioning.cmake` as a simplified one-stop OS agnostic file for updating versions [@aidenjbass](https://github.com/aidenjbass)
- GitHub Action have been created for continuous integration and release builds for desktop platforms [@aidenjbass](https://github.com/aidenjbass)

### Changed
- RetroFE now follows calendar versioning followed by a patch number in the format YYMM.X [@aidenjbass](https://github.com/aidenjbass)
- The first two digits are the year, and the second two are the month. 2504 codifies a release from April 2025. [@aidenjbass](https://github.com/aidenjbass)
- Major YYMM versions are denoted by major features released in a new month [@aidenjbass](https://github.com/aidenjbass)
- Hotfix, minor bugfix and feature releases will have the addition of a suffix "X". [@aidenjbass](https://github.com/aidenjbass)
- Additionally for internal development builds, an appended git hash allows us to keep track of specific binary versions [@aidenjbass](https://github.com/aidenjbass)
- README.md has undergone a significant rewrite with cohesive and updated build instructions for Windows, Linux and macOS [@aidenjbass](https://github.com/aidenjbass)

### Fixed
- Made the Xcode project portable [@aidenjbass](https://github.com/aidenjbass)
- CMake can now statically build macOS builds again [@aidenjbass](https://github.com/aidenjbass)

## [2503.0] - 2025-03-09
### This version details the technical overhaul that [Gstexperimentappsink](https://github.com/CoinOPS-Official/RetroFE/pull/221) made undertaken by [@inigomontoya](https://github.com/inigomontoya)
### Added
- Initial implementation of attract mode launch (`attraceModeLaunch`). Includes settings for `attractModeLaunchTime` and timeout interruption by key/button press.
- Function to terminate a process and all child processes for attract mode.
- `timeSpent` reloadable text tag to track time spent playing a particular item.
- `reloadableHiscores` component for high score tables with attributes like `maxRows` and `excludedColumns`.
- `quickList` and `quickList2` features.
- `perspective` layout tag.
- Support for `randomPlaylist` and updates to `randomStart`.
- Smart pointers for GStreamer to avoid manual cleanup.

### Changed
- Refined caching mechanism for images and animations, introducing per-monitor caching.
- Reverted multiple changes including:
  - "fix new fineMatchingFile"
  - "added scalemode attribute to image tags"
  - "use ImageBuilder for all images"
  - "sort LayerComponents_ by layer"
  - "small AnimationEvents optimization"
  - "Allow % for layout positioning."
  - "ViewInfo optimization"
- Adjustments to `TNQueue` and `GStreamerVideo` for improved performance and stability.
- Linux-specific fixes for plugins, volume behavior, and launcher initialization.
- Refactored `scroll()` to ensure correct allocation and deallocation of components.
- Improved logging for the Launcher class and GStreamer.
- Optimized `reloadableScrollingText` and `reloadableHiscores` rendering.
- Simplified image class and animation handling.

### Fixed
- Issue with video stuttering when restarting.
- Compilation issues on Windows and Linux.
- Incorrect `textFallback` behavior.
- Bugs in `ScrollingList` and its optimizations.
- Reloadable text format attribute (`textFormat`) handling.
- Issues with `randomStart` and playlist behavior.
- Crash issues when shutting down `videopool` on exit.
- Texture reuse and clearing mechanism for videos.
- Undefined behavior when performing `back()` on an empty string.

### Removed
- `D3D11Memory` from hardware video acceleration caps due to instability on some systems.
- Extraneous alpha checks.
- Old scrolling text behavior when high scores are enabled.


## [10.34.5] - 2024-02-18

### Added
- added default.conf for default game info [@monkofthefunk](https://github.com/monkofthefunk)
- Append build number to RetroFE binary when building [@aidenjbass](https://github.com/aidenjbass)
- A plethora of CLI options [@aidenjbass](https://github.com/aidenjbass)
- -help to show all [@aidenjbass](https://github.com/aidenjbass)
- -createcollection, generates a default collection structure with global/local [@aidenjbass](https://github.com/aidenjbass)
- -rebuilddatabase, rebuilds the database without a full initialisation [@aidenjbass](https://github.com/aidenjbass)
- -showusage, print a list of all settings [@aidenjbass](https://github.com/aidenjbass)
- -showconfig, print a list of current settings and properties [@aidenjbass](https://github.com/aidenjbass)
- -createconfig, generate a default settings.conf and associated readme [@aidenjbass](https://github.com/aidenjbass)
- -dump, dump current settings and properties to a file [@aidenjbass](https://github.com/aidenjbass)
- Pass all global settings via CLI in [-key] [value] format [@aidenjbass](https://github.com/aidenjbass)
- layoutFromAnotherCollection key, search in layouts/\<layout>/collections/\<collectionName>/layout/ [@aidenjbass](https://github.com/aidenjbass)
- File existence checks for settings.conf etc, could result in seg faults under certain conditions [@aidenjbass](https://github.com/aidenjbass)
- If init fails GUI textbox will show instead of just logging to console [@aidenjbass](https://github.com/aidenjbass)

### Changed
- Moved all global settings to GlobalOpts data class [@aidenjbass](https://github.com/aidenjbass)
- Existence checks for start and exit scripts [@aidenjbass](https://github.com/aidenjbass)
- Placed jbKey's behind a jukebox key to not bother mapping [@aidenjbass](https://github.com/aidenjbass)
- Rebuilt the layout getter in pageBuilder [@aidenjbass](https://github.com/aidenjbass)

## [10.34.4] - 2024-02-03 - The Forgotten Universe Atarashii Release

### Added
- local launchers that are restricted to their own collections [@inigomontoya](https://github.com/inigomontoya)
- thread pool to speed up page updates and scrolling (Windows only) [@inigomontoya](https://github.com/inigomontoya)
- csv list to lastPlayedSkipCollection [@aidenjbass](https://github.com/aidenjbass)

### Changed
- assert window focus after SDL initialization [@inigomontoya](https://github.com/inigomontoya)

### Fixed
- refined behavior of video restart/pause logic [@inigomontoya](https://github.com/inigomontoya)
- general optimization pass [@inigomontoya](https://github.com/inigomontoya)
- layouts will no longer try and load images/videos/menus on monitors that don't exist [@inigomontoya](https://github.com/inigomontoya)
- ensured when splash video ends, frontend will be entered [@inigomontoya](https://github.com/inigomontoya)
- fixed letterUp/letterDown to do what they say [@inigomontoya](https://github.com/inigomontoya)
- fixed sizable memory leak [@inigomontoya](https://github.com/inigomontoya)

## [10.34.3] - 2023-08-27

### Added
- collection setting "layout" that overides global setting, so different themes can be loaded under one collection menu [@monkofthefunk](https://github.com/monkofthefunk)
- toggelGameInfo, toggleCollectionInfo, toggleBuildInfo controls with onGameInfoEnter/Exit etc. events [@monkofthefunk](https://github.com/monkofthefunk)
- setting infoExitOnScroll to call on*InfoExit if open when scrolling [@monkofthefunk](https://github.com/monkofthefunk)
- reload of menu and reloadables after game exit to change any art affected by settings or meta change [@monkofthefunk](https://github.com/monkofthefunk)
- randomStart to start on random game [@monkofthefunk](https://github.com/monkofthefunk)
- animateDuringGame to turn off animated marquee animation during game launch [@monkofthefunk](https://github.com/monkofthefunk)
- settings control toggle and settingsCollectionPlaylist to determine what collection/playlist it's in [@monkofthefunk](https://github.com/monkofthefunk)
- random playlist to randomStart [@monkofthefunk](https://github.com/monkofthefunk)
- random highlight on first playlist scroll if randomStart [@monkofthefunk](https://github.com/monkofthefunk)
- ability to have ..: blank playlist to allow for multiple playlists in global setting toggle, uses autoPlaylist and cyclePlaylist in setting collection [@monkofthefunk](https://github.com/monkofthefunk)
- implemented file cache [@inigomontoya](https://github.com/inigomontoya)
- configurable quitCombo in controls.conf, example quitCombo = joyButton6,joyButton7 [@inigomontoya](https://github.com/inigomontoya)
- ability for collections to have their own launcher within the folder; also a menu collection can have a setting "menuFromCollectionLaunchers=true" to build menu based off of collections with launcher.conf [@monkofthefunk](https://github.com/monkofthefunk)
- access to selected collection's art via mode="system" on relodables [@monkofthefunk](https://github.com/monkofthefunk)
- ability to build Universal2 binaries on MacOS via xcodeproj [@aidenjbass](https://github.com/aidenjbass)
- added %COLLECTION_PATH% to launchers, resolves to full path to collection [@inigomonoya] (https://github.com/inigomontoya)
- controllerComboSettings, controller/keyboard combo that toggles settings [@inigomonoya] (https://github.com/inigomontoya)
- HardwareVideoAccel support on macOS (video toolbox) [@aidenjbass](https://github.com/aidenjbass)

### Changed
- changed video pause/restart behavior, also videos will now pause by default if out of view, disablePauseOnScroll=true in settings.conf for global override. [@inigomontoya](https://github.com/inigomontoya)
- reworked and streamlined gstreamer implementation [@inigomontoya](https://github.com/inigomontoya)
- changed ignoreDefaultLayout to defaultToCurrentLayout for collection setting that prevents default to root layout if a collection one not found also fixes reloading the page if still on same layout [@monkofthefunk](https://github.com/monkofthefunk)
- update cmake version and add some speed flags to linux build [@monkofthefunk](https://github.com/monkofthefunk)
- log macros and reworking to make completely benign [@inigomontoya](https://github.com/inigomontoya)
- removed Video.cpp and Video.h, all videos use VideoBuilder [@inigomontoya](https://github.com/inigomontoya)
- early initialization of gstreamer on launch instead of when first video plays [@inigomontoya](https://github.com/inigomontoya)
- removed dependency on dirent.h [@inigomontoya](https://github.com/inigomontoya)
- reworked xml metadata import function, should be quicker [@inigomontoya](https://github.com/inigomontoya)
- changed mouse warp to resolution extremes in favour of relativemousemode [@aidenjbass](https://github.com/aidenjbass)
- removed mouseX and mouseY from config [@aidenjbass](https://github.com/aidenjbass)


### Fixed
- playlist order if menuSort=false for sub items [@arghs15](https://github.com/arghs15)
- randomSelect [@monkofthefunk](https://github.com/monkofthefunk)
- second display using first aspect ratio when in different monitor postions; add default ""monitor"" to layout element; increased layouts to 5 [@monkofthefunk](https://github.com/monkofthefunk)
- global settings no overidding collection playlistCycle/firstPlaylist in a collection's settings [@monkofthefunk](https://github.com/monkofthefunk)
- playlist to retain order if menuSort=false; also improved startup speed when loading playlists [@monkofthefunk](https://github.com/monkofthefunk)
- linux warnings [@inigomontoya](https://github.com/inigomontoya)
- reallocating videos when going back a page [@monkofthefunk](https://github.com/monkofthefunk)
- not going into collections when they don't exist [@monkofthefunk](https://github.com/monkofthefunk)
- blip between playlists for reloadable videos [@monkofthefunk](https://github.com/monkofthefunk)
- skip loading new reloadable (causing blip) if the asset path is the same [@monkofthefunk](https://github.com/monkofthefunk)
- infoExitOnScroll [@monkofthefunk](https://github.com/monkofthefunk)
- toggle off on attract mode [@monkofthefunk](https://github.com/monkofthefunk)
- adding and removing from global playlist; fixed starting on favorite playlist on faorites collection [@monkofthefunk](https://github.com/monkofthefunk)
- case where 3 games in a menu with 4 each side of selected would dysync from selected relodables [@monkofthefunk](https://github.com/monkofthefunk)
- global favorites adding and removing and how it affects current collection's favorites list [@monkofthefunk](https://github.com/monkofthefunk)
- using mp3 in video element [@monkofthefunk](https://github.com/monkofthefunk)
- collection animation crash due to memleak overfix [@monkofthefunk](https://github.com/monkofthefunk)
- clearing runtime playlist cycle cache upon collection change [@monkofthefunk](https://github.com/monkofthefunk)
- resuming on menu with less items then size of menu [@monkofthefunk](https://github.com/monkofthefunk)
- global menu item removal menu update [@monkofthefunk](https://github.com/monkofthefunk)
- settings collection asset refresh [@monkofthefunk](https://github.com/monkofthefunk)
- manual sort after refactor [@monkofthefunk](https://github.com/monkofthefunk)
- setting jump blip when within same collection; while not breaking menu reload upon new collection [@monkofthefunk](https://github.com/monkofthefunk)
- toggling close the info upon going to settings; fixed toggling back to last from settings in different collection [@monkofthefunk](https://github.com/monkofthefunk)
- random start playlist to exclude favorites and lastplayed [@monkofthefunk](https://github.com/monkofthefunk)
- mouse handling and fullscreen on MacOS [@aidenjbass](https://github.com/aidenjbass)
- unloadSDL functionality fixed, for raspberry pi devices [@bluestand2006] (https://github.com/bluestang2006)
- assert window input focus after SDL is initialized [@inigomontoya] (https://github.com/inigomontoya)
 
## [10.34.2] - 2023-06-29

### Added
- start.bat/sh exit.bat/sh launch on start/exit of retrofe [@monkofthefunk](https://github.com/monkofthefunk)
- animated marquee to linux [@monkofthefunk](https://github.com/monkofthefunk)
- random video on start for screensaver [@monkofthefunk](https://github.com/monkofthefunk)
- prevCycleCollection button [@monkofthefunk](https://github.com/monkofthefunk)
- collection setting specific "ignoreDefaultLayout=false" for collection menu support to use current collection layout and not load default if the selected collection doesn't have one [@monkofthefunk](https://github.com/monkofthefunk)
- textFallback to menu to turn off if no image/video found [@monkofthefunk](https://github.com/monkofthefunk)
- to layout animationDoneRemove remove video, to free up a video slot after a one time video is done [@monkofthefunk](https://github.com/monkofthefunk)
- disableVideoRestart to turn off a layout's "restart" on video selected [@monkofthefunk](https://github.com/monkofthefunk)
- screensaver setting that will quit upon mouse move or button down [@monkofthefunk](https://github.com/monkofthefunk)

### Changed
- update version *.2 [@monkofthefunk](https://github.com/monkofthefunk)
- changed pixel format from I420 to NV12 [@inigomontoya](https://github.com/inigomontoya)
- make quit combo and collection cycle faster but cycle button isn't as sensitive [@monkofthefunk](https://github.com/monkofthefunk)
- move playlist cycle and back button out of idle check [@monkofthefunk](https://github.com/monkofthefunk)
- made collection cycle button a bit more responsive [@monkofthefunk](https://github.com/monkofthefunk)
- changes to facilitate compiling 64-bit instead of puny 32-bit [@inigomontoya](https://github.com/inigomontoya)
- big optimization pass, refactored functions for speed/clarity [@inigomontoya](https://github.com/inigomontoya)
- if LEDBlinkyDirectory is not set or absent, LEDBlinky is disabled [@inigomontoya](https://github.com/inigomontoya)
- built SDL2 and SDL2_image DLLs from scratch [@inigomontoya](https://github.com/inigomontoya)

### Fixed
- bug with marquee and launching lastplayed would play video audio during game [@monkofthefunk](https://github.com/monkofthefunk)
- collection setting autoPlaylist to go to upon nav [@monkofthefunk](https://github.com/monkofthefunk)
- to use the default layout.xml and the layout-#.xml if not found in collection layouts [@monkofthefunk](https://github.com/monkofthefunk)
- crash on settings reboot [@monkofthefunk](https://github.com/monkofthefunk)
- removal of favorite in favorite playlist, update menu [@monkofthefunk](https://github.com/monkofthefunk)
- back button not updating collection cycle postion [@monkofthefunk](https://github.com/monkofthefunk)
- not showing splash when screensaver enabled [@monkofthefunk](https://github.com/monkofthefunk)
- collection cycle button and lastKeyPressed logic [@monkofthefunk](https://github.com/monkofthefunk)
- multi monitor start up crash [@monkofthefunk](https://github.com/monkofthefunk)
- select sound making background play sound while game launched [@monkofthefunk](https://github.com/monkofthefunk)
- size_t warnings [@monkofthefunk](https://github.com/monkofthefunk)
- size_t exception [@monkofthefunk](https://github.com/monkofthefunk)
- don't add videos to monitor=2+ if display doesn't exist [@monkofthefunk](https://github.com/monkofthefunk)
- layer count tide to numScreens setting, also allow single screen to allow second monitor show desktop [@monkofthefunk](https://github.com/monkofthefunk)
- updating layout defined controls upon "back" menu/layout change [@monkofthefunk](https://github.com/monkofthefunk)
- case where if Windows scaling was set to anything other than 100%, SDL would create an inappropriate size window and let windows scale it [@inigomontoya](https://github.com/inigomontoya)
- memory leak in CollectionInfoBuilder [@monkofthefunk](https://github.com/monkofthefunk)
- case in layouts where "center" in from or to fields would always be 0. [@inigomontoya](https://github.com/inigomontoya)
- attract mode entering right collection and fast scroll to launch game [@monkofthefunk](https://github.com/monkofthefunk), [@inigomontoya](https://github.com/inigomontoya)
- occasional entering of wrong game [@inigomontoya](https://github.com/inigomontoya)

## [10.34.1] - 2023-05-21

### Added

- vsync; fix if vSync is yes, skip SDL_Delay fps limiter [@inigomontoya](https://github.com/inigomontoya)
- HardwareVideoAccel flag to settongs.conf [@inigomontoya](https://github.com/inigomontoya)
- avdec_h265 properties [@inigomontoya](https://github.com/inigomontoya)
- "max-threads" property [@inigomontoya](https://github.com/inigomontoya)
- missing gstvideoconvertscale.dll [@inigomontoya](https://github.com/inigomontoya)
- wait for playbin's state to actually be GST_STATE_NULL [@inigomontoya](https://github.com/inigomontoya)
- playlist display menu [@monkofthefunk](https://github.com/monkofthefunk)
- MaxThreads property of avdech264 and avdech265 configurable in settings.conf [@inigomontoya](https://github.com/inigomontoya)
- don't cycle playlist on particular lists if on "street fighter and capcom fighters" or "street fighter" [@monkofthefunk](https://github.com/monkofthefunk)
- remeber selected rom between playlists within a collection [@monkofthefunk](https://github.com/monkofthefunk)
- to exit when displaying splash [@BP](), [@monkofthefunk](https://github.com/monkofthefunk)
- to filter logs via setting "log=" see CoinOPS-Official/RetroFE/pull/9 [@monkofthefunk](https://github.com/monkofthefunk)
- to only perform layout action if setting "action=" set and "setting=" in layout match see pull/9 [@monkofthefunk](https://github.com/monkofthefunk)
- support for multiple controls1-9.conf for launcher to set controls upon launch [@monkofthefunk](https://github.com/monkofthefunk)
- collection's settings.conf to override global settings.conf firstPlaylist, attractModeCyclePlaylist, cyclePlaylist, attractModeSkipPlaylist [@monkofthefunk](https://github.com/monkofthefunk)
- disabled favtoggle while in favs [@BP]()
- globalFavLast=yes to save favorites and lastplayed to "Favorites" collection that all collections can access or not, using cyclePlaylist in collection [@monkofthefunk](https://github.com/monkofthefunk)
- %X_RES% and %Y_RES% variables for horizontal and vertical in settings.conf for AndyBurn [@inigomontoya](https://github.com/inigomontoya)
- %CMD% variable for executable field in launcher files, will expand to system's cmd.exe [@inigomontoya](https://github.com/inigomontoya)
- ScaleQuality to settings.conf.  0=nearest 1=linear 2=anisotropic.  Defaults to 1.  https://wiki.libsdl.org/SDL2/SDL_HINT_RENDER_SCALE_QUALITY [@inigomontoya](https://github.com/inigomontoya)
- playlistNextEnter/Exit and playlistPrevEnter/Exit layout events upon playlist navigation changes [@monkofthefunk](https://github.com/monkofthefunk)
- Utils::getEnvVar [@monkofthefunk](https://github.com/monkofthefunk)
- env X_RES_0,X_RES_1, X_RES_2, etc. for horizontal0=%X_RES% and vertical0=%Y_RES% in settings.conf [@monkofthefunk](https://github.com/monkofthefunk)
- collection's settings to support for multiple settings1-9.conf for theme specific media folder alias' used by the theme [@monkofthefunk](https://github.com/monkofthefunk)
- ability to specify SDL render driver to use, SDLRenderDriver in settings.conf.  Acceptable values are direct3d11, opengl [@inigomontoya](https://github.com/inigomontoya)
- logging to report the SDL render driver being used [@inigomontoya](https://github.com/inigomontoya)
- MuteVideo in settings.conf, yes will mute GStreamer audio [@inigomontoya](https://github.com/inigomontoya)
- metadata sorting and setting sortType year, manufacturer, developer, genre, numberPlayers, numberButtons, ctrlType, joyWays, rating, score [@monkofthefunk](https://github.com/monkofthefunk)
- a playlist based on a metadata field to sort by that field [@monkofthefunk](https://github.com/monkofthefunk)
- menu jump for meta sorted playlists [@monkofthefunk](https://github.com/monkofthefunk)
- to only perform layout action if on playlist="" might support coma seperated list [@monkofthefunk](https://github.com/monkofthefunk)
- attractModeFast = yes to control via setting; defaults to no [@monkofthefunk](https://github.com/monkofthefunk)
- support for action if playlist is within comma seperated playlist="pl1,pl2,p3" to make one action work for many playlists [@monkofthefunk](https://github.com/monkofthefunk)
- attractModeSkipPlaylist support multiple comma seperated [@monkofthefunk](https://github.com/monkofthefunk)
- reloadable image of type="position" from 1-27 position/1.png to display position in the games list [@monkofthefunk](https://github.com/monkofthefunk)
- postMessage on start and quit to alert other programs about its current state [@inigomontoya](https://github.com/inigomontoya)
- property <item selected="true" restart="true"> to restart video upon scroll [@monkofthefunk](https://github.com/monkofthefunk)
- playCount sort; lastPlayed text shows "1 year(s) 1 month(s) 1 day(s) ago" [@monkofthefunk](https://github.com/monkofthefunk)
- <reloadableImage/Video randomLimit="3"... will look for "filename - <1-3>.png" [@monkofthefunk](https://github.com/monkofthefunk)
- controls.conf kiosk=F3 mode that locks navigation, playlist toggle, fav toggle; settings.conf kiosk=true to start on first playlist locked, above toggle to is optional to set [@monkofthefunk](https://github.com/monkofthefunk)
- "controls - <text>.conf" to be applied based on theme <layout controls="<text>", useful for horizontal and vertical [@monkofthefunk](https://github.com/monkofthefunk)
- reloadable type playCount and text lastPlayed that shows date; stores values in lastplayed; sorts last played list by last played [@monkofthefunk](https://github.com/monkofthefunk)
- highPriority setting to set process to above normal [@monkofthefunk](https://github.com/monkofthefunk)
- setting to randomly choose a layout, for those who can't decide. randomLayout=layoutARCADES,layoutCASCADING,layoutSPIN [@monkofthefunk](https://github.com/monkofthefunk)
- jbPause to pause attract mode [@monkofthefunk](https://github.com/monkofthefunk)
- animated marquee during game [@monkofthefunk](https://github.com/monkofthefunk)
- controllerComboExit uses "start" and "back" [@monkofthefunk](https://github.com/monkofthefunk)
- adaptive="true" SDL blend mode for menus and images [@inigomontoya](https://github.com/inigomontoya), [@monkofthefunk](https://github.com/monkofthefunk)
- unique window names "retrofe0" [@inigomontoya](https://github.com/inigomontoya)
- cycleCollection = ThemeConsoles,ThemeArcades / cycleCollection = W that will cycle through collections with different layouts [@monkofthefunk](https://github.com/monkofthefunk)
- enabled SDL logging to report backend being used, either direct3d or opengl [@inigomontoya](https://github.com/inigomontoya)
- prevent fav toggle on special playlists "street fighter and capcom fighters" or "street fighter" [@monkofthefunk](https://github.com/monkofthefunk)

### Changed
- changed parsing behavior for settings.conf, will now accept "on" as well as "yes" and "true" [@inigomontoya](https://github.com/inigomontoya)
- changed SDL screen config logic to iterate over number of screens preventing iterations beyond number of found displays [@inigomontoya](https://github.com/inigomontoya)
- disabled name jump for lastplayed [@monkofthefunk](https://github.com/monkofthefunk)
- revert multi firing fix, affects onMenuJump animations [@monkofthefunk](https://github.com/monkofthefunk)
- remove acceleration from attract mode scroll [@monkofthefunk](https://github.com/monkofthefunk)
- improved meta data import checks to increase startup speed [@monkofthefunk](https://github.com/monkofthefunk)
- improved media player waiting intialization to use threads to increase startup speed [@inigomontoya](https://github.com/inigomontoya)
- improve restart video fast scroll flicker [@monkofthefunk](https://github.com/monkofthefunk)
- move lastplayed and playcount info into it's own playCount file [@monkofthefunk](https://github.com/monkofthefunk)
- changed X_RES_# to H_RES_# and Y_RES_# to V_RES_# and horizontal0/verical0=envvar [@monkofthefunk](https://github.com/monkofthefunk)

### Fixed
- displaying of JPG images; added libjpeg-8.dll [@inigomontoya](https://github.com/inigomontoya)
- refactor processNewBuffer function to reduce time spent inside mutexes [@inigomontoya](https://github.com/inigomontoya)
- ensure VideoBus_ is initialized before handoff [@inigomontoya](https://github.com/inigomontoya)
- reloadable video playlist type [@monkofthefunk](https://github.com/monkofthefunk)
- fav item removal to  select next one [@monkofthefunk](https://github.com/monkofthefunk)
- playlist menu after globalFavLast merge [@monkofthefunk](https://github.com/monkofthefunk)
- remebering last favorite selected in playlist [@monkofthefunk](https://github.com/monkofthefunk)
- removed playlistNextEnter from fav toggle; ensured other RETROFE_PLAYLIST_REQUEST didn't trigger playlistNext/PrevExit events [@monkofthefunk](https://github.com/monkofthefunk)
- showing playlist menu upon item fav toggle and letter jump/random (don't trigger on playlist enter events) [@monkofthefunk](https://github.com/monkofthefunk)
- some log errors to warnings [@monkofthefunk](https://github.com/monkofthefunk)
- multi firing of onMenuIdle [@monkofthefunk](https://github.com/monkofthefunk)
- jump item meta miss match with previous item [@monkofthefunk](https://github.com/monkofthefunk)
- start and end position [@monkofthefunk](https://github.com/monkofthefunk)
- reloadable media on playlist change [@monkofthefunk](https://github.com/monkofthefunk)
- crashing due to sort; fixed restart video happens when becomming selected and not from; fixed last played text for current day [@monkofthefunk](https://github.com/monkofthefunk)
- sort to be more supportive of sort types [@monkofthefunk](https://github.com/monkofthefunk)
- playcount as an reloadableimage [@monkofthefunk](https://github.com/monkofthefunk)
- reload playcount after launch [@monkofthefunk](https://github.com/monkofthefunk)
- don't trigger video restart if scrolling fast [@monkofthefunk](https://github.com/monkofthefunk)
- reload playcount after launch for images [@monkofthefunk](https://github.com/monkofthefunk)
- playCount import [@monkofthefunk](https://github.com/monkofthefunk)
- playlist menu flash after playing a lastplayed game [@monkofthefunk](https://github.com/monkofthefunk)
- kiosk image toggle [@monkofthefunk](https://github.com/monkofthefunk)
- random layout to load layout-1... [@monkofthefunk](https://github.com/monkofthefunk)
- sound playing in background during game from animated marquee changes, and retrofe waiting on marquee video to end before starting [@monkofthefunk](https://github.com/monkofthefunk)
- adaptive feature affect on marquee menu videos (inigonmontoya, monkofthefunk)
- pausing alpha=0 videos without black flicker during fast scroll [@monkofthefunk](https://github.com/monkofthefunk)
- fade in video due to stoping video update [@monkofthefunk](https://github.com/monkofthefunk)
- second screen menu update upon last played game launch and order reshuffle [@monkofthefunk](https://github.com/monkofthefunk)
- high GPU due to marquee rendering thread on game launch [@monkofthefunk](https://github.com/monkofthefunk)
