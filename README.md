<h1 style="
  display: inline-block !important;
  font-size: 20rem;
">
  <img
    src="./Package/Environment/Common/RetroFE.png"
    alt="Icon"
    height="140px"
    style="
      display: inline-block !important;
      height: 3.5rem;
      margin-right: 1rem;
    "
  />
  <span style="position: relative; bottom: 0.7rem;">
    RetroFE
  </span>
</h1>

[Project Discord](https://discord.gg/dpcsP8Hm9W) | [GitHub Wiki](https://github.com/CoinOPS-Official/RetroFE/tree/master/docs) | [Changelog](CHANGELOG.md)

RetroFE is a cross-platform desktop frontend designed for MAME cabinets and game centers, with a focus on simplicity and customization. 
This repository is actively maintained and hundreds of commits ahead of the original RetroFE project. 
It is designed for use within CoinOPS builds, bringing with it a significant increase in performance, optimisations, and available feature set. 

It's licensed under the terms of the GNU General Public License, version 3 or later (GPLv3).

## What's so special about this fork?
* Performance and optimisations
	* 64-bit codebase
    * C++20 as standard
	* Modern render engine; DX11 for Windows, Metal for MacOS
	* Hardware accelerated video support for Windows and Linux
	* VSync and support for high refresh rate
	* Metadata database build time reduced
	* File caching to prevent drive lashing
	* RAM usage reduced by 70%
* Features
	* Ability to start on random item; fed up of seeing the same game every time?
 	* Robust video marquee and 2nd screen support	 
	* Upgraded attract mode
	* Upgraded favouriting system; global and local favourites
	* Start and exit scripts; run programs such as steam at retrofe launch
	* In depth logging system; 7 logging levels
	* Kiosk mode; lock things down for kids or cleanliness
 	* Local Hiscores integration with hi2txt   
	* And much more!

## System Requirements
* OS
    * Windows (10 or higher)
    * Linux (AppImage requires libc 2.38 or higher)
    * macOS (11 Big Sur or higher)
	* Unix-like systems other than Linux are not officially supported but may work
* Processor
    * A modern CPU (2014 or later) is highly recommended
* Graphics
    * A reasonably modern graphics card (Direct3D 11+ / OpenGL 4+ / Metal on MacOS)

## Building

SDL3 is the standard build on all platforms. RetroFE uses unmodified upstream
SDL3 libraries; no SDL patches are required.

On Windows, install Visual Studio 2022 with C++/Windows SDK, CMake 3.24+, and
GStreamer's MSVC x64 runtime and development packages, then run:

```powershell
git submodule update --init --recursive
./RetroFE/Source/Build.ps1
```

This downloads verified official SDL development archives, generates
`RetroFE/Build/retrofe.sln`, builds and tests RetroFE, and stages the matching
runtime in `RetroFE/Build/bin/Release`.

Linux and macOS use CMake with installed SDL3 packages or pinned upstream
source builds. See [build, dependency and packaging instructions](RetroFE/Source/BUILDING.md)
for all platforms. The standalone interop prototypes are optional developer tools.

#   Optional #

###   Creating a test environment

A launchable test environment can be created with the following commands 

	python3 Scripts/Package.py --os=windows/linux/mac --build=full

Copy your live RetroFE system to any folder of your choosing. Files can be found in `Artifacts/{os}/RetroFE`

### Set $RETROFE_PATH via Environment variable 

RetroFE will load it's media and configuration files relative to where the binary file is located. This allows the build to be portable. If you want RetroFE to load your configuration from a fixed location regardless of where your install is copy your configuration there and set $RETROFE_PATH. Note this will work if you start RetroFE from the command line.

	vi ~/.bash_profile
	export RETROFE_PATH=/your/new/retrofe


### Set RETROFE_PATH via flat file 

Depending on your version of OS X the GUI will read user defined Environment variables from [another place](http://stackoverflow.com/questions/135688/setting-environment-variables-in-os-x). If you find this dificult to setup you can get around it by creating a text file in your HOME directory: /Users/<you>/.retrofe with one line no spaces: /your/new/retrofe. This will also work in Linux. RetroFE's configuration search order is 1st: ENV, Flat file, and executable location.

	echo /your/new/retrofe > ~/.retrofe

### Fix libpng iCCP warnings

The issue is with the png files that are being used with the Artwork. Libpng is pretty touchy about it. You can get rid of these messages with a handy tool called pngcrush found on sourceforge and github.

Error message:
	
	libpng warning: iCCP: known incorrect sRGB profile


Install pngcrush on Mac:    (linux use apt-get ?)
	
	brew install pngcrush


Use pngcrush to Find and repair pngs: 
	
	find /usr/local/opt/retrofe/collections -type f -iname '*.png' -exec pngcrush -ow -rem allb -reduce {} \;
