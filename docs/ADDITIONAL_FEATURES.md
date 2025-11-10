# Additional Features
[Back](README.md)

Overtime RetroFE's need for extra features has grown due to community demand and due to the nature of open source, every line can be changed.

## Ambient Mode
### Why and What

Ambient Mode allows your arcade cabinet to assume a low-key presence in a room. For example, in a living room setting, you might not want the cabinet to be a focal point all the time. Ambient Mode allows the cabinet to recede to the background without powering it all the way off.

Essentially, Ambient Mode acts as a screensaver, but instead of the typical "screensaver" functionality (which is already used elsewhere in the project and pulls its artwork from the `/collections` folder), it provides a visually engaging backdrop without disrupting the system.

When enabled:

The `quitCombo` controller combo button will go to ambient mode instead of exiting RetroFE.
While in ambient mode, images from the `/ambient`directory will be displayed on the main screen, and rotated periodically
To exit ambient mode, the controller combo button OR the action button will return you to the main RetroFE menu.

### Configuration
To set up Ambient Mode

- Create a Directory
    - Create a directory named `/ambient` in the RetroFE root directory.
    - Populate the `/ambient` directory with images you want to display.


- Configure Settings

In the settings.conf file, add the following configuration options

```
controllerComboExitAction = AMBIENT
ambientModeMinutesPerImage = 2
```

`controllerComboExitAction` controls what happens when you press select+start in RetroFE:
    - QUIT: quits RetroFE
    - AMBIENT: puts RetroFE in Ambient mode
    - NONE: nothing happens. Use this if you want to prevent users from quitting RetroFE via controller (you can still quit with the keyboard)

`ambientModeMinutesPerImage`: Optional. This option defines how often the image should change (in minutes). If left unspecified, the default is 20 minutes.

### LEDBlinky Integration
When activated, Ambient Mode triggers LEDBLinky's "Screensaver" mode - so you want your LEDs/buttons to do something particular (including just go dark),
configure LEDBlinky's "Screensaver" mode.

### Marquee Support
If you have a dual-monitor setup, Ambient Mode supports a marquee display on the second monitor:

- How It Works
    - When an image is displayed on the main screen, the system will check for a corresponding marquee image on the second monitor.
    - It looks for a file named `imageName_marquee.ext` (e.g., `sunset_marquee.png`) where `imageName` matches the image displayed on the main screen.
- Fallback
    - If no corresponding marquee image is found, a random marquee image from the `/ambient` directory will be displayed instead.