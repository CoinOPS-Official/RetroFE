# Controls
[Back](README.md)

The `controls.conf` file, located in your RetroFE directory, contains the
controls for your RetroFE frontend. Multiple keys can be assigned to a
single action, separated by a `,`

Example:

 Up = Keypad 8, Up

This will assign both the Keypad 8 (up arrow on your keypad) and the up
arrow to RetroFE's Up control.

## RetroFE Control Keys

### Essential
These keys must be defined for RetroFE to boot
| Control | Description |
|---------|-------------|
| `up` | Scrolls menu up (for vertical menus) |
| `down` | Scrolls menu up (for vertical menus) |
| `left` | Scrolls menu left (for horizontal menus) |
| `right` | Scrolls menu right (for horizontal menus) |
| `select` | Selects the active menu item |
| `back` | Leaves current menu |
| `quit` | Exits the frontend |

### Basic Navigation
Most users will want to use these keys to move around a build
| Control | Description |
|---------|-------------|
| `nextCyclePlaylist` | Switches to the next playlist in the cyclePlaylist set. |
| `prevCyclePlaylist` | Switches to the previous playlist in the cyclePlaylist set. |
| `letterUp` | Scrolls menu to the previous item in the alphabet |
| `letterDown` | Scrolls menu to next item in the alphabet |

### Advanced Navigation
| Control | Description |
|---------|-------------|
| `playlistUp` | Scrolls to the previous playlist in a collection in vertical layouts. |
| `playlistDown` |Scrolls to the next playlist in a collection in vertical layouts. |
| `playlistLeft` | Scrolls to the previous playlist in a collection in horizontal layouts. |
| `playlistRight` | Scrolls to the next playlist in a collection in horizontal layouts. |
| `collectionUp` | Scrolls to the previous collection in vertical layouts. Will enter that collection if enterOnCollection is true. |
| `collectionDown` | Scrolls menu next collection in vertical layouts. Will enter that collection based if enterOnCollection is true. |
| `collectionLeft` | Scrolls to the previous collection in horizontal layouts. Will enter that collection if enterOnCollection is true. |
| `collectionRight` | Scrolls menu next collection in horizontal layouts. Will enter that collection based if enterOnCollection is true. |
| `pageUp` | Scrolls menu back by a page |
| `pageDown` | Scrolls menu forward by a page |
| `prevPlaylist` | Switches to the previous playlist. |
| `nextPlaylist` | Switches to the next playlist. |
| `cyclePlaylist` | Switches to the next playlist in the cyclePlaylist set. Still functional, but has been replaced by nextCyclePlaylist. |
| `prevCycleCollection` | Switches to the previous playlist in the cycleCollection set. |
| `cycleCollection` | Switches to the next playlist in the cycleCollection set. |

### Jukebox Mode
These keys are used to define jukebox controls when RetroFE operates in a jukebox environment
| Control | Description |
|---------|-------------|
| `jbFastForward1m` | Jukebox fast forward 1 minute |
| `jbFastRewind1m` | Jukebox fast rewind 1 minute |
| `jbFastForward5p` | Jukebox fast forward 5% |
| `jbFastRewind5p` | Jukebox fast rewind 5% |
| `jbPause` | Jukebox pause |
| `jbRestart` | Jukebox restart |

### Button Combos
These keys are used to define button combos with two keycodes. The first value defines the shift modifier
| Control | Description |
|---------|-------------|
| `quitCombo` | Quits RetroFE |
| `settingsCombo` | Switches to the settings playlist defined in settings.conf `controllerComboSettings` |
| `gameInfoCombo` | Toggles Game Info with a button combo |
| `collectionInfoCombo` | Toggles Collection Info with a button combo |
| `buildInfoCombo` | Toggles Build Info with a button combo |

### Favorites
| Control | Description |
|---------|-------------|
| `addPlaylist` | Adds current item to the favorites playlist |
| `removePlaylist` | Removes current item from the favorites playlist |
| `favPlaylist` | Switches to the favorites playlist |
| `togglePlaylist` | Toggle adding current item to favorites playlist |
| `random` | Selects a random item |

### Admin
| `settings` | Switches to the settings playlist |
| `reboot` | Reboot RetroFE and refresh config |
| `kiosk` | Toggle kiosk mode |

### Deadzone
| Control | Description |
|---------|-------------|
| `deadZone` | Defines the dead zone for analog inputs between 0 and 100, with 100 blocking all analog input |


| `menu` | |
| `saveFirstPlaylist` | |
| `quickPlaylist` |  |


## Gamepad/Joystick codes

- `X` = Controller number (e.g. `joy0` is the first connected gamepad)
- `Y` = Button or hat number on controller `X`

| Keycode| Description |
|--------|-------------|
| `joyXButtonY` | button Y on joystick X |
| `joyXHatYLeftUp` | hat (D-pad) diagonally up-left (hat Y on joystick X) |
| `joyXHatYLeft` | hat (D-pad) left direction (hat Y on joystick X) |
| `joyXHatYLeftDown` | hat (D-pad) diagonally down-left (hat Y on joystick X) |
| `joyXHatYUp` | hat (D-pad) up direction (hat Y on joystick X) |
| `joyXHatYDown` | hat (D-pad) down direction (hat Y on joystick X) |
| `joyXHatYRightUp` | hat (D-pad) diagonally up-right (hat Y on joystick X) |
| `joyXHatYRight` | hat (D-pad) right direction (hat Y on joystick X) |
| `joyXHatYRightDown` | hat (D-pad) diagonally down-right (hat Y on joystick X) |
| `joyXAxis0+` | Left analog stick, horizontal axis right (joystick X) |
| `joyXAxis0-` | Left analog stick, horizontal axis left (joystick X) |
| `joyXAxis1+` | Left analog stick, vertical axis down (joystick X) |
| `joyXAxis1-` | Left analog stick, vertical axis up (joystick X) |
| `joyXAxis2+` | Right analog stick, horizontal axis right (joystick X) |
| `joyXAxis2-` | Right analog stick, horizontal axis left (joystick X) |
| `joyXAxis3+` | Right analog stick, vertical axis down (joystick X) |
| `joyXAxis3-` | Right analog stick, vertical axis up (joystick X) |

If X is omitted, RetroFE will accept input from all controllers.
(version 0.8.13+) 

## Mouse Codes

| Keycode | Description |
|---------|-------------|
| `mouseButtonleft` | the left mouse button |
| `mouseButtonMiddle` | the middle mouse button |
| `mouseButtonRight` | the right mouse button |
| `mouseButtonX1` | the X1 mouse button |
| `mouseButtonX2` | the X2 mouse button |

 
## Keyboard Codes

| Keycode | Description |
|---------|-------------|
| 0 | |
| 1 | |
| 2 | |
| 3 | |
| 4 | |
| 5 | |
| 6 | |
| 7 | |
| 8 | |
| 9 | |
| A | |
| AC Back | the Back key (application control keypad) |
| AC Bookmarks | the Bookmarks key (application control keypad) |
| AC Forward | the Forward key (application control keypad) |
| AC Home | the Home key (application control keypad) |
| AC Refresh | the Refresh key (application control keypad) |
| AC Search | the Search key (application control keypad) |
| AC Stop | the Stop key (application control keypad) |
| Again | the Again key (Redo) |
| AltErase | Erase-Eaze |
| ' | |
| Application | the Application / Compose / Context Menu (Windows) key |
| AudioMute | the Mute volume key |
| AudioNext | the Next Track media key |
| AudioPlay | the Play media key |
| AudioPrev | the Previous Track media key |
| AudioStop | the Stop media key) |
| B | |
| \\ | Located at the lower left of the return key on ISO keyboards and at the right end of the QWERTY row on ANSI keyboards. Produces REVERSE SOLIDUS (backslash) and VERTICAL LINE in a US layout, REVERSE SOLIDUS and VERTICAL LINE in a UK Mac layout, NUMBER SIGN and TILDE in a UK Windows layout, DOLLAR SIGN and POUND SIGN in a Swiss German layout, NUMBER SIGN and APOSTROPHE in a German layout, GRAVE ACCENT and POUND SIGN in a French Mac layout, and ASTERISK and MICRO SIGN in a French Windows layout. |
| Backspace | |
| BrightnessDown | the Brightness Down key |
| BrightnessUp | the Brightness Up key |
| C | |
| Calculator | the Calculator key |
| Cancel | |
| CapsLock | |
| Clear | |
| Clear / Again | |
| , | |
| Computer | the My Computer key |
| Copy | |
| CrSel | |
| CurrencySubUnit | the Currency Subunit key |
| CurrencyUnit | the Currency Unit key |
| Cut | |
| D | |
| DecimalSeparator | the Decimal Separator key |
| Delete | |
| DisplaySwitch | display mirroring/dual display switch, video mode switch |
| Down | the Down arrow key (navigation keypad) |
| E | |
| Eject | the Eject key |
| End | |
| = | |
| Escape | the Esc key |
| Execute | |
| ExSel | |
| F | |
| F1 | |
| F10 | |
| F11 | |
| F12 | |
| F13 | |
| F14 | |
| F15 | |
| F16 | |
| F17 | |
| F18 | |
| F19 | |
| F2 | |
| F20 | |
| F21 | |
| F22 | |
| F23 | |
| F24 | |
| F3 | |
| F4 | |
| F5 | |
| F6 | |
| F7 | |
| F8 | |
| F9 | |
| Find | |
| G | |
| \` | Located in the top left corner (on both ANSI and ISO keyboards). Produces GRAVE ACCENT and TILDE in a US Windows layout and in US and UK Mac layouts on ANSI keyboards, GRAVE ACCENT and NOT SIGN in a UK Windows layout, SECTION SIGN and PLUS-MINUS SIGN in US and UK Mac layouts on ISO keyboards, SECTION SIGN and DEGREE SIGN in a Swiss German layout (Mac: only on ISO keyboards), CIRCUMFLEX ACCENT and DEGREE SIGN in a German layout (Mac: only on ISO keyboards), SUPERSCRIPT TWO and TILDE in a French Windows layout, COMMERCIAL AT and NUMBER SIGN in a French Mac layout on ISO keyboards, and LESS-THAN SIGN and GREATER-THAN SIGN in a Swiss German, German, or French Mac layout on ANSI keyboards. |
| H | |
| Help | |
| Home | |
| I | |
| Insert | insert on PC, help on some Mac keyboards (but does send code 73, not 117) |
| J | |
| K | |
| KBDIllumDown | the Keyboard Illumination Down key |
| KBDIllumToggle | the Keyboard Illumination Toggle key |
| KBDIllumUp | the Keyboard Illumination Up key |
| Keypad 0 | the 0 key (numeric keypad) |
| Keypad 00 | the 00 key (numeric keypad) |
| Keypad 000 | the 000 key (numeric keypad) |
| Keypad 1 | the 1 key (numeric keypad) |
| Keypad 2 | the 2 key (numeric keypad) |
| Keypad 3 | the 3 key (numeric keypad) |
| Keypad 4 | the 4 key (numeric keypad) |
| Keypad 5 | the 5 key (numeric keypad) |
| Keypad 6 | the 6 key (numeric keypad) |
| Keypad 7 | the 7 key (numeric keypad) |
| Keypad 8 | the 8 key (numeric keypad) |
| Keypad 9 | the 9 key (numeric keypad) |
| Keypad A | the A key (numeric keypad) |
| Keypad & | the & key (numeric keypad) |
| Keypad @ | the @ key (numeric keypad) |
| Keypad B | the B key (numeric keypad) |
| Keypad Backspace | the Backspace key (numeric keypad) |
| Keypad Binary | the Binary key (numeric keypad) |
| Keypad C | the C key (numeric keypad) |
| Keypad Clear | the Clear key (numeric keypad) |
| Keypad ClearEntry | the Clear Entry key (numeric keypad) |
| Keypad : | the : key (numeric keypad) |
| Keypad , | the Comma key (numeric keypad) |
| Keypad D | the D key (numeric keypad) |
| Keypad && | the && key (numeric keypad) |
| \\\| | \\\| key (numeric keypad) |
| Keypad Decimal | the Decimal key (numeric keypad) |
| Keypad / | the / key (numeric keypad) |
| Keypad E | the E key (numeric keypad) |
| Keypad Enter | the Enter key (numeric keypad) |
| Keypad = | the = key (numeric keypad) |
| Keypad = (AS400) | the Equals AS400 key (numeric keypad) |
| Keypad ! | the ! key (numeric keypad) |
| Keypad F | the F key (numeric keypad) |
| Keypad \> | the Greater key (numeric keypad) |
| Keypad # | the # key (numeric keypad) |
| Keypad Hexadecimal | the Hexadecimal key (numeric keypad) |
| Keypad { | the Left Brace key (numeric keypad) |
| Keypad ( | the Left Parenthesis key (numeric keypad) |
| Keypad \< | the Less key (numeric keypad) |
| Keypad MemAdd | the Mem Add key (numeric keypad) |
| Keypad MemClear | the Mem Clear key (numeric keypad) |
| Keypad MemDivide | the Mem Divide key (numeric keypad) |
| Keypad MemMultiply | the Mem Multiply key (numeric keypad) |
| Keypad MemRecall | the Mem Recall key (numeric keypad) |
| Keypad MemStore | the Mem Store key (numeric keypad) |
| Keypad MemSubtract | the Mem Subtract key (numeric keypad) |
| Keypad - | the - key (numeric keypad) |
| Keypad \\\* | the \\\* key (numeric keypad) |
| Keypad Octal | the Octal key (numeric keypad) |
| Keypad % | the Percent key (numeric keypad) |
| Keypad . | the . key (numeric keypad) |
| Keypad + | the + key (numeric keypad) |
| Keypad +/- | the +/- key (numeric keypad) |
| Keypad ^ | the Power key (numeric keypad) |
| Keypad } | the Right Brace key (numeric keypad) |
| Keypad ) | the Right Parenthesis key (numeric keypad) |
| Keypad Space | the Space key (numeric keypad) |
| Keypad Tab | the Tab key (numeric keypad) |
| key (numeric keypad) | |
| Keypad XOR | the XOR key (numeric keypad) |
| L | |
| Left Alt | alt, option |
| Left Ctrl | |
| Left | the Left arrow key (navigation keypad) |
| \[ | |
| Left GUI | windows, command (apple), meta |
| Left Shift | |
| M | |
| Mail | the Mail/eMail key |
| MediaSelect | the Media Select key |
| Menu | |
| \- | |
| ModeSwitch | I'm not sure if this is really not covered by any of the above, but since there's a special KMOD_MODE for it I'm adding it here |
| Mute | |
| N | |
| Numlock | the Num Lock key (PC) / the Clear key (Mac) |
| O | |
| Oper | |
| Out | |
| P | |
| PageDown | |
| PageUp | |
| Paste | |
| Pause the Pause / Break key | |
| . | |
| Power | The USB document says this is a status flag, not a physical key - but some Mac keyboards do have a power key. |
| PrintScreen | |
| Prior | |
| Q | |
| R | |
| Right Alt | alt gr, option |
| Right Ctrl | |
| Return | the Enter key (main keyboard) |
| Return | |
| Right GUI | windows, command (apple), meta |
| Right | the Right arrow key (navigation keypad) |
| \] | |
| Right Shift | |
| S | |
| ScrollLock | |
| Select | |
| ; | |
| Separator | |
| / | |
| Sleep | the Sleep key |
| Space | the Space Bar key(s) |
| Stop | |
| SysReq | the SysReq key |
| T | |
| Tab | the Tab key |
| ThousandsSeparator | the Thousands Separator key |
| U | |
| Undo | |
| Up | the Up arrow key (navigation keypad) |
| V | |
| VolumeDown | |
| VolumeUp | |
| W | |
| WWW | the WWW/World Wide Web key |
| X | |
| Y | |
| Z | |
| # | ISO USB keyboards actually use this code instead of 49 for the same key, but all OSes I've seen treat the two codes identically. So, as an implementor, unless your keyboard generates both of those codes and your OS treats them differently, you should generate SDL_SCANCODE_BACKSLASH instead of this code. As a user, you should not rely on this code because SDL will never generate it with most (all?) keyboards. |
| & | |
| \* | |
| @ | |
| ^ | |
| : | |
| $ | |
| ! | |
| \> | |
| # | |
| ( | |
| \< | |
| % | |
| \+ | |
| ? | |
| ) | |
| \_ | |

These codes were taken from <https://wiki.libsdl.org/SDL_Keycode>

[Back](README.md)
