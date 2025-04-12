#!/bin/bash

# Set the target directory
TARGET_DIR="${PROJECT_DIR}/../ThirdPartyMac/GStreamer.framework/Versions/1.0/lib"

# Delete .a and .la files from the main directory
echo "Deleting .a and .la files in $TARGET_DIR..."
find "$TARGET_DIR" -type f \( -name "*.a" -o -name "*.la" \) -exec rm -f {} +

# Check if the gstreamer-1.0 directory exists and delete files there too
GSTREAMER_DIR="$TARGET_DIR/gstreamer-1.0"
if [ -d "$GSTREAMER_DIR" ]; then
  echo "Deleting .a and .la files in $GSTREAMER_DIR..."
  find "$GSTREAMER_DIR" -type f \( -name "*.a" -o -name "*.la" \) -exec rm -f {} +
else
  echo "No gstreamer-1.0 directory found in $TARGET_DIR."
fi

echo "Deletion completed."
