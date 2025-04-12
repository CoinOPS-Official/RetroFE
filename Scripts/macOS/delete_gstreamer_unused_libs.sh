#!/bin/bash

# Required suffix for the path
REQUIRED_SUFFIX="RetroFE.app/Contents/Frameworks/GStreamer.framework/Versions/1.0/lib"

# Allow the user to pass a custom base path as the first argument
USER_BASE_PATH="$1"

# If no path is provided, use the current directory
if [[ -z "$USER_BASE_PATH" ]]; then
    USER_BASE_PATH="."
fi

# Ensure the provided path ends with the required suffix
if [[ "$USER_BASE_PATH" != *"$REQUIRED_SUFFIX" ]]; then
    APP_BUNDLE_PATH="$USER_BASE_PATH/$REQUIRED_SUFFIX"
else
    APP_BUNDLE_PATH="$USER_BASE_PATH"
fi

# Ensure the final path exists
if [[ ! -d "$APP_BUNDLE_PATH" ]]; then
    echo "Error: The path '$APP_BUNDLE_PATH' does not exist."
    exit 1
fi

# List of all dylibs in your app bundle
# Paste contents of gstreamer_dylibs.txt produced by get_gstreamer_libs_used
REQUIRED_LIBRARIES=(
    libgstvulkan-1.0.0.dylib
    libgstvulkan-1.0.dylib
    libMoltenVK.dylib

    libgstbadaudio-1.0.0.dylib
    libgstapp-1.0.0.dylib
    libgstvideo-1.0.0.dylib
    libgstnet-1.0.0.dylib
    libgobject-2.0.0.dylib
    libgstaudio-1.0.0.dylib
    libgstreamer-1.0.0.dylib
    libgstinsertbin-1.0.0.dylib
    libgstallocators-1.0.0.dylib
    libgstphotography-1.0.0.dylib
    libglib-2.0.0.dylib
    libgsttranscoder-1.0.0.dylib
    libgstcheck-1.0.0.dylib
    libgstplayer-1.0.0.dylib
    libgsttag-1.0.0.dylib
    libgstfft-1.0.0.dylib
    libgstsctp-1.0.0.dylib
    libgstcontroller-1.0.0.dylib
    libgstpbutils-1.0.0.dylib
    libgstsdp-1.0.0.dylib
    libgstmpegts-1.0.0.dylib
    libgstwebrtcnice-1.0.0.dylib
    libgstcodecparsers-1.0.0.dylib
    libgstgl-1.0.0.dylib
    libgstrtp-1.0.0.dylib
    libgstwebrtc-1.0.0.dylib
    libgstplay-1.0.0.dylib
    libgmodule-2.0.0.dylib
    libintl.8.dylib
    libgstriff-1.0.0.dylib
    libgstrtsp-1.0.0.dylib
    libgstbase-1.0.0.dylib
    libnice.10.dylib
    libz.1.dylib
    libffi.7.dylib
    libpcre2-8.0.dylib
    libgstrtspserver-1.0.0.dylib
    liborc-0.4.0.dylib
    libgio-2.0.0.dylib
    libcrypto.1.1.dylib
    libavfilter.8.dylib
    libbz2.1.dylib
    libswresample.4.dylib
    libavutil.57.dylib
    libavformat.59.dylib
    libavcodec.59.dylib

    gstreamer-1.0/libgstplayback.dylib
    gstreamer-1.0/libgstautodetect.dylib
    gstreamer-1.0/libgstapp.dylib
    gstreamer-1.0/libgstcoreelements.dylib
    gstreamer-1.0/libgsttypefindfunctions.dylib
    gstreamer-1.0/libgstapplemedia.dylib
    gstreamer-1.0/libgstaudioparsers.dylib
    gstreamer-1.0/libgstaudioconvert.dylib
    gstreamer-1.0/libgstvideoparsersbad.dylib
    gstreamer-1.0/libgstisomp4.dylib
    gstreamer-1.0/libgstvideofilter.dylib
    gstreamer-1.0/libgstaudioresample.dylib
    gstreamer-1.0/libgstvideoconvertscale.dylib
    gstreamer-1.0/libgstlibav.dylib
    gstreamer-1.0/libgstosxaudio.dylib
    gstreamer-1.0/libgstvolume.dylib
)

# Find all .dylib files in the app bundle, including subdirectories
find "$APP_BUNDLE_PATH" -type f -name "*.dylib" | while read -r dylib; do
    # Get the relative path of the dylib file
    dylib_relative_path="${dylib#$APP_BUNDLE_PATH/}"

    # Check if the dylib is required (if not, delete it)
    if [[ ! " ${REQUIRED_LIBRARIES[@]} " =~ " ${dylib_relative_path} " ]]; then
        echo "Deleting unneeded library: $dylib_relative_path"
        rm -f "$dylib"  # Delete the file
    fi
done

echo "Cleanup completed!"