# Launchers
[Back](README.md)


A launcher config file describes how to launch a program (i.e. emulator,
application, or game) when a launchable menu item is selected.

See below for a list of supported configuration properties. Launcher
options

| Property          | Description                                                                 |
|-------------------|-----------------------------------------------------------------------------|
| executable        | Path of where the executable exists                                         |
| arguments         | Arguments to pass when executing the launcher (i.e. ROM name)               |
| liveHiscores      | Connect to MAME's openhi2txt live-score output while the game is running    |
| liveHiscoresPort  | Local TCP port for live scores (default: 32123)                              |

    executable = D:/Emulators/Nestopia/nestopia.exe
    arguments  = "%ITEM_FILEPATH%"

For a MAME launcher using the openhi2txt live-score plugin:

    executable = emulators/mame/mame64.exe
    arguments = "%ITEM_NAME%"
    liveHiscores = true
    liveHiscoresPort = 32123

For MAME software-list items, put the runtime identity in the collection's
existing `meta.xml` entry:

    <game name="my-frontend-item">
        <mamemachine>genesis</mamemachine>
        <mamesoftwarelist>megadriv</mamesoftwarelist>
        <mamesoftware>tecmobb</mamesoftware>
    </game>

The frontend item name remains free to follow the collection's artwork and
collision-avoidance conventions. It is not treated as an OpenHi2txt definition
name. A launcher can use the metadata directly:

    executable = emulators/mame/mame64.exe
    arguments = "%MAME_MACHINE%" "%MAME_SOFTWARE%"
    liveHiscores = true

This example launches as `mame64.exe genesis tecmobb`. The software-list name
remains `megadriv`; it identifies the hash catalog, not necessarily the machine
argument accepted by MAME.

If these metadata tags are absent, the earlier ROM-hash resolver remains as a
compatibility fallback for `%MAME_SOFTWARELIST%` and `%MAME_SOFTWARE%`. It
matches a loose ROM or ZIP contents against MAME's hash XMLs. It cannot always
infer the machine driver because one software list may be usable by several
machines; `%MAME_MACHINE%` therefore requires explicit metadata.

A literal machine argument also remains valid:

    arguments = genesis "%MAME_SOFTWARE%"

MAME reports the authoritative machine (`genesis`), software list (`megadriv`),
and software (`tecmobb`) to OpenHi2txt after launch. OpenHi2txt—not RetroFE—
resolves that structured identity to a decoder definition. Normal content
resolution does not split the frontend filename.
For compatibility with existing collections, a zero-byte placeholder item may
fall back to an exact `<machine>,<software>:` entry in `hiscore.dat`; non-empty
ROM files are always identified by their contents.

When the matched software entry declares writable data areas such as
`<dataarea name="sram" size="16384">`, RetroFE retains the area name, size,
part, interface, and slot feature. For live NVRAM inputs it correlates those
areas with openhi2txt's planned source size and sends the matching storage
names as hints to MAME. MAME still verifies the actual live NVRAM device and
requires its serialized size to match before observing any requested ranges.

Live scores are opt-in per launcher. RetroFE connects only to localhost,
reconnects if MAME starts first or temporarily disconnects, and stops the
connection when the launched game exits.

%ITEM_FILEPATH% is a reserved variable name. See the variables table
below for other variables that may be used. Also note the quotes around
"%ITEM_FILEPATH%" to help not confuse the executable from thinking that
an item with spaces as multiple arguments.

Assuming that "Super Mario Bros" was the selected item, the frontend
will attempt to execute:

    "D:/Emulators/Nestopia/nestopia.exe" "D:/ROMs/Nintendo/Super Mario Bros.nes".

**PS**: You can also use relative paths (relative to the root folder of
RetroFE)

    executable = ../Emulators/Nestopia/nestopia.exe
    arguments  = "%ITEM_FILEPATH%"

Launcher variables

| Variable               | Description                 | Translated Example                    |
|------------------------|-----------------------------|---------------------------------------|
| %ITEM_FILEPATH%        | Full item path              | D:/ROMs/Nintendo/Super Mario Bros.nes |
| %ITEM_NAME%            | The item name               | Super Mario Bros                      |
| %ITEM_FILENAME%        | Filename without path       | Super Mario Bros.nes                  |
| %ITEM_DIRECTORY%       | Folder where file exists    | D:/ROMs/Nintendo                      |
| %ITEM_COLLECTION_NAME% | Name of collection for item | Nintendo Entertainment System         |
| %RETROFE_PATH%         | Folder location of Frontend | D:/Frontends/RetroFE                  |
| %RETROFE_EXEC_PATH%    | Location of RetroFE         | D:/Frontends/RetroFE/RetroFE.exe      |
| %COLLECTION_PATH%      | Full path to collection     | D:/Frontends/collections/Genesis      |
| %MAME_MACHINE%         | MAME machine driver from item metadata | genesis |
| %MAME_SOFTWARELIST%    | MAME software-list short name from metadata or hash fallback | megadriv |
| %MAME_SOFTWARE%        | MAME software short name from metadata or hash fallback | tecmobb |
| %CMD% (Windows only)   | Path to system cmd.exe      | C:/Windows/system32/cmd.exe           |

More elaborate example:

    # Have fceux load a save state automatically for the ROM when started
    executable = D:/Emulators/fceux/fceux.exe
    arguments  = "%ITEM_FILEPATH%" -loadstate "%ITEM_DIRECTORY%/%ITEM_NAME%.fcs"

[Back](README.md)
