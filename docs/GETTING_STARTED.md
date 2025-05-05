# Getting Started
[Back](README.md)

Getting started with RetroFE is as simple as following these steps:

1. Download RetroFE for your OS.
2. Edit the [global settings.conf](GLOBAL_SETTINGS.md) file.
3. Configure your [controls.conf](CONTROLS.md) file.
4. Run RetroFE to ensure the frontend loads and exits correctly.
5. Add, edit, or delete [collections](COLLECTIONS.md).
6. Re-run RetroFE to check everything works.

## RetroFE Root Directory Structure

| File / Folder | Description |
|---------------|-------------|
| `controls.conf` | Controller definition (e.g. up, down, select, back) |
| `log.txt` | Log output |
| `meta.db` | Game database (year, manufacturer, genre, players, etc.) |
| `RetroFE.{lnk/AppImage/app}` | RetroFE executable |
| `settings.conf` | Global settings (display options, layout, base paths, etc.) |
| `/collections/` | Game lists, menus, artwork, and ROMs |
| `/retrofe/` | Windows-specific libraries needed for RetroFE to run (includes retrofe.exe) |
| `/launchers/` | Configuration files for launchers (emulators) |
| `/layouts/` | Themes/layouts for the frontend |
| `/meta/` | Files to import into meta.db |


# Detailed Setup Guide
## Installation
Once you've copied RetroFE to your chosen directory, you can test the installation by running the `retrofe` executable. RetroFE comes with a pre-installed Sega Genesis system, which includes a single game for your first test.

## Configuration
### Step 1: Edit the Global Settings
The first configuration step involves editing the global settings file:  
`settings.conf`.
This file defines your screen settings, global theme, base paths, and other system-wide settings.

### Step 2: Configure the Controls
The second step is to edit the `controls.conf` file to define the controls for your front-end. By default, the "select" key is space, not enter as some might expect.

---

## Adding Collections
RetroFE starts with two basic [collections](COLLECTIONS.md), but you can easily add more. For example, let's set up the **Nintendo Entertainment System (NES)** collection:

1. **Create a new collection**:
   - Navigate to the `RetroFE/collections` directory.
   - Run the following command to create the collection:  
     `../retrofe -createcollection "Nintendo Entertainment System"`

2. **Add ROMs and artwork**:
   - Download a NES ROM set and place the ROMs in:  
     `RetroFE/collections/Nintendo Entertainment System/roms`
   - Download the system artwork (e.g., device image, logo, and video) and place them in:  
     `RetroFE/collections/Nintendo Entertainment System/system_artwork`
   - Download game-specific artwork (e.g., front artwork, logos, screenshots) and place them in:  
     `RetroFE/collections/Nintendo Entertainment System/medium_artwork`

3. **Configure the new collection**:
   - Edit the **RetroFE/collections/Nintendo Entertainment System/settings.conf** file to match the following:

     ```ini
     list.extensions = nes
     launcher = NES
     ```

     The first line defines the ROM file extension as `.nes`, which should match the ROM files in the `/roms` directory. The second line sets the launcher for this collection (in this case, "NES").

4. **Configure the launcher**:
   - Edit the **RetroFE/launchers/NES.conf** file to define the launcher. For example, using MAME 0.162:

     ```ini
     executable = mame
     arguments = nes -cart "%ITEM_FILEPATH%"
     ```

     This configuration launches the game using the command:
     `mame nes -cart "collections/Nintendo Entertainment System/roms/Willow (USA).nes"`

5. **Add the collection to the main menu**:
   - Edit the **RetroFE/collections/Main/menu.txt** file to include the new collection. Add the following line:

     ```plaintext
     Nintendo Entertainment System
     ```

6. **Test the collection**:
   - After making all changes, test your newly added collection by running the **retrofe** executable:  
     `RetroFE/retrofe`

---

[Back](README.md)
