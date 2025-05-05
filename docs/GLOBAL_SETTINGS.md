# Global settings.conf
[Back](README.md)

## Overview
The global `settings.conf` file, located in the root directory, controls the global behavior of the RetroFE instance. Below is a list of available configuration parameters you can define in this file.

## Alternate Configuration Files
To support scripted reconfiguration or multiple setup profiles, RetroFE also supports additional configuration files named sequentially: `settings1.conf`, `settings2.conf`, up to `settings16.conf`.

## Command-Line Overrides
Additionally, all settings can be passed as command-line options using the format `-optionName value`. For example: `-LOG ALL -muteVideo yes` would enable logging in all categories and mute video audio. Command-line options have the highest priority. Any value specified on the command line will override the corresponding setting in all settings.conf or settingsX.conf files.

## LOGGING OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `log` | `NONE` | `STRING` | Set logging level, any combo of ERROR,INFO,NOTICE,WARNING,DEBUG,FILECACHE or ALL or NONE. Use - to invert logging (ALL, -INFO) | ✅ |
| `dumpProperties` | `false` | `BOOLEAN` | Dump contents of properties to txt in current directory | ✅ |

## DISPLAY OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `numScreens` | `1` | `INTEGER` | Defines the number of monitors used | |
| `fullscreen` | `true` | `BOOLEAN` | Run the frontend in fullscreen | |
| `horizontal` | `stretch` | `STRING` | Pixel width INT or STRETCH | |
| `vertical` | `stretch` | `STRING` | Pixel height INT or STRETCH | |
| `fullscreenX` | `true` | `BOOLEAN` | Run the frontend in fullscreen for monitor x | |
| `horizontalX` | `""` | `INTEGER` | Pixel width for monitor x | |
| `verticalX` | `""` | `INTEGER` | Pixel height for monitor x | |
| `screenNumX` | `""` | `INTEGER` | Define which monitor x is which display window, Screen numbers start at 0! | |
| `mirrorX` | `false` | `BOOLEAN` | Divides monitor x into two halves | |
| `rotationX` | `0` | `INTEGER` | Rotation of monitor x (0, 1, 2, 3) | |

## WINDOW OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `windowBorder` | `false` | `BOOLEAN` | Show window border | |
| `windowResize` | `false` | `BOOLEAN` | Allow window to be resized | |
| `fps` | `60` | `INTEGER` | Requested FPS while in an active state | |
| `fpsIdle` | `60` | `INTEGER` | Request FPS while in an idle state | |
| `hideMouse` | `true` | `BOOLEAN` | Defines whether the mouse cursor is hidden | |
| `animateDuringGame` | `true` | `BOOLEAN` | Pause animated marquees while in the game | ✅ |

## VIDEO OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `videoEnable` | `true` | `BOOLEAN` | Defines whether video is rendered | |
| `videoLoop` | `0` | `INTEGER` | Number of times to play video, 0 forever | |
| `disableVideoRestart` | `false` | `BOOLEAN` | Pauses video while scrolling | ✅ |
| `disablePauseOnScroll` | `false` | `BOOLEAN` | Restart video when selected | ✅ |

## RENDERER OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `vsync` | `false` | `BOOLEAN` | Vertical synchronization | |
| `hardwareVideoAccel` | `false` | `BOOLEAN` | Hardware decoding | ✅ |
| `avdecMaxThreads` | `2` | `INTEGER` | Number of threads for avdec software decoding | ✅ |
| `muteVideo` | `false` | `BOOLEAN` | Video playback is muted | ✅ |
| `sdlRenderDriver` | `direct3d` | `STRING` | Set renderer (direct3d, direct3d11, direct3d12, opengl, opengles2, opengles, metal, and software) | ✅ |
| `scaleQuality` | `1` | `INTEGER` | Scaling quality (0, 1, 2) | ✅ |
| `highPriority` | `false` | `BOOLEAN` | RetroFE Windows process priority | ✅ |
| `unloadSDL` | `false` | `BOOLEAN` | Close SDL when launching a game, MUST be true for RPI | |
| `minimizeOnFocusLoss` | `false` | `BOOLEAN` | Minimize RetroFE when focus is lost | |
| `avdecThreadType` | `2` | `INTEGER` | Type of threading in the case of software decoding (1=frame, 2=slice) | |
| `glSwapInterval` | `1` | `INTEGER` | OpenGL Swap Interval (0=immediate updates, 1=synchronized vsync, -1=adaptive vsync) | |

## CUSTOMIZATION OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `layout` | `Arcades` | `STRING` | Theme to be used in RetroFE, a folder name in /layouts | |
| `randomLayout` | `""` | `MSTRING` | Randomly choose a layout on launch, CSV list of layout names | ✅ |
| `firstPlaylist` | `arcades` | `STRING` | Start on this playlist if available | |
| `autoPlaylist` | `all` | `STRING` | Start on this playlist when entering a collection if available | |
| `quickListCollectionPlaylist` | `""` | `STRING` | Jump to playlist by way of quickList key | |
| `cyclePlaylist` | `""` | `MSTRING` | Set of playlists that can be cycled through, CSV list of playlist names | |
| `firstCollection` | `""` | `STRING` | Start on this collection if available | ✅ |
| `cycleCollection` | `""` | `MSTRING` | Set of collections that can be cycled through, CSV list of collection names | ✅ |
| `lastPlayedSize` | `10` | `INTEGER` | Size of the auto-generated last played playlist, 0 to disable | |
| `lastPlayedSkipCollection` | `""` | `MSTRING` | Skip CSV list of collections being added to last played | |
| `action` | `""` | `STRING` | If action=<something> and the action has setting=<something> then perform animation | |
| `enterOnCollection` | `false` | `BOOLEAN` | Enter the collection when using collection up/down controls | |
| `backOnCollection` | `false` | `BOOLEAN` | Move to the next/previous collection when using the collectionUp/Down/Left/Right buttons | |
| `startCollectionEnter` | `false` | `BOOLEAN` | Enter the first collection on RetroFE boot | |
| `exitOnFirstPageBack` | `false` | `BOOLEAN` | Exit RetroFE when the back button is pressed on the first page | |
| `rememberMenu` | `true` | `BOOLEAN` | Remember the last highlighted item if re-entering a menu | |
| `backOnEmpty` | `false` | `BOOLEAN` | Automatically back out of empty collection | |
| `subsSplit` | `false` | `BOOLEAN` | Split merged collections based on subs (true) or sort as one list (false) | |
| `cfwLetterSub` | `false` | `BOOLEAN` | Jump subs in a collection by sub instead of by the letter of the item | ✅ |
| `prevLetterSubToCurrent` | `false` | `BOOLEAN` | Jump to the start of the current letter instead of the previous letter if jump to letter enabled | |
| `randomStart` | `false` | `BOOLEAN` | Start on a random item when RetroFE boots | ✅ |
| `randomPlaylist` | `false` | `BOOLEAN` | Start on a random playlist when RetroFE boots | |
| `kiosk` | `false` | `BOOLEAN` | Start on the first playlist in cyclePlaylist with navigation and favorites locked, can be toggled with a setting in controls.conf | ✅ |
| `globalFavLast` | `false` | `BOOLEAN` | Save last played and favorites to a new collection | ✅ |
| `infoExitOnScroll` | `false` | `BOOLEAN` | Hide info text boxes when scrolling | ✅ |
| `jukebox` | `false` | `BOOLEAN` | Enables mapping of jukebox controls | |
| `fixedResLayouts` | `false` | `BOOLEAN` | Enables the use of fixed resolution layouts ie layout1920x1080.xml | ✅ |
| `screensaver` | `false` | `BOOLEAN` | Enables screensaver mode | |


## ATTRACT MODE OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `attractModeCyclePlaylist` | `false` | `BOOLEAN` | Cycle through all playlists or defined in cyclePlaylist | |
| `attractModeTime` | `19` | `INTEGER` | Number of seconds to wait before scrolling to another random point | |
| `attractModeNextTime` | `19` | `INTEGER` | Number of seconds to wait before scrolling to another random point while attract mode is active | |
| `attractModePlaylistTime` | `300` | `INTEGER` | Number of seconds to wait before attract mode jumps to another playlist, 0 to lock | |
| `attractModeSkipPlaylist` | `""` | `MSTRING` | Skip CSV list of playlists while in attract mode | ✅ |
| `attractModeCollectionTime` | `300` | `INTEGER` | Number of seconds before attract mode switches to the next collection, 0 to lock | |
| `attractModeSkipCollection` | `""` | `MSTRING` | Skip CSV list of collections while in attract mode | |
| `attractModeMinTime` | `100` | `INTEGER` | Minimum number of milliseconds attract mode will scroll | |
| `attractModeMaxTime` | `1600` | `INTEGER` | Maximum number of milliseconds attract mode will scroll | |
| `attractModeFast` | `false` | `BOOLEAN` | Scroll(false) or jump(true) to the next random point while in attract mode | ✅ |
| `attractModeLaunch` | `false` | `BOOLEAN` | When in attract mode will launch games for a time configured by attractModeLaunchRunTime, default 30 sec | |
| `attractModeLaunchRunTime` | `30` | `INTEGER` | If attractModeLaunch = true, the length of time a launched item will run | |
| `attractModeLaunchMinMaxScrolls` | `3,5` | `MSTRING` | If attractModeLaunch = true, comma separated min and max number of scrolls before launch | |

## INPUT OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `collectionInputClear` | `false` | `BOOLEAN` | Clear input queue on collection change | |
| `playlistInputClear` | `false` | `BOOLEAN` | Clear input queue on playlist change | |
| `jumpInputClear` | `false` | `BOOLEAN` | Clear input queue while jumping through the menu | |
| `controllerComboExit` | `true` | `BOOLEAN` | Close RetroFE with the controller combo set in controls.conf | ✅ |
| `controllerComboSettings` | `false` | `BOOLEAN` | Open settings playlist with the controller combo set in controls.conf | ✅ |
| `settingsCollectionPlaylist` | `Arcades:settings` | `STRING` | Used by settings toggle to go to the playlist in collection:playlist format, defaults to settings.txt in the current collection | ✅ |
| `servoStikEnabled` | `false` | `BOOLEAN` | Enable ServoStik support | |

## METADATA OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `metaLock` | `true` | `BOOLEAN` | Locks RetroFE from looking for XML changes and uses meta.db, faster loading when true | ✅ |
| `overwriteXML` | `false` | `BOOLEAN` | Allows metadata XMLs to be overwritten by files in a collection | |
| `showParenthesis` | `true` | `BOOLEAN` | Show item information between () | |
| `showSquareBrackets` | `true` | `BOOLEAN` | Show item information between [] | |

## WINDOWS ONLY OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `ledBlinkyDirectory` | `""` | `PATH` | Path to LEDBlinky installation | |

## MEDIA SEARCH PATH OPTIONS
| Option | Default | Type | Description | CoinOPS Added Feature |
|--------|---------|------|-------------|-----------------------|
| `baseMediaPath` | `""` | `PATH` | Path to media if stored outside the build | |
| `baseItemPath` | `""` | `PATH` | Path to items if stored outside the build | |

[Back](README.md)