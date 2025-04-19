#!/bin/bash
# Get list of everything RetroFE has a hook on
dylib_paths=$(lsof -c RetroFE -F n | grep '\.dylib' | sed 's/^n//')
# Filter to just GStreamer related
filtered_paths=$(echo "$dylib_paths" | grep 'RetroFE.app/Contents/Frameworks/GStreamer.framework/')

# Echo to a file 
echo "$filtered_paths" > gstreamer_dylibs.txt

# Extract the first line from gstreamer_dylibs.txt as path_root
path_root=$(head -n 1 gstreamer_dylibs.txt)

# Extract the path up to RetroFE.app from path_root
path_to_retrofe=$(echo "$path_root" | sed -n 's|\(.*RetroFE.app\)/.*|\1|p')

# We add these as we always need these for intel macs and rosetta
echo "\n" >> gstreamer_dylibs.txt
echo "${path_to_retrofe}/Contents/Frameworks/GStreamer.framework/Versions/1.0/lib/libgstvulkan-1.0.0.dylib" >> gstreamer_dylibs.txt
echo "${path_to_retrofe}/Contents/Frameworks/GStreamer.framework/Versions/1.0/lib/libgstvulkan-1.0.dylib" >> gstreamer_dylibs.txt
echo "${path_to_retrofe}/Contents/Frameworks/GStreamer.framework/Versions/1.0/lib/libMoltenVK.dylib" >> gstreamer_dylibs.txt

# Remove the path prefix from every line in gstreamer_dylibs.txt
sed -i.bak "s|${path_to_retrofe}/Contents/Frameworks/GStreamer.framework/Versions/1.0/lib/||g" gstreamer_dylibs.txt

# Cleanup: Remove the backup file created by sed (on macOS, sed creates a .bak file)
rm -f gstreamer_dylibs.txt.bak