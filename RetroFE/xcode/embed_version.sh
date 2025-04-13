# Xcode: Set version and build number from CMake versioning.cmake
# ---------------------------------------------------------------
#
# This script invokes versioning.cmake to extract the version and build
# information, then updates the Info.plist with the appropriate values.

set -e

VERSIONING_CMAKE="${PROJECT_DIR}/../CMake/versioning.cmake"
PLIST="${PROJECT_DIR}/../xcode/Info.plist"

# Ensure file exists
if [[ ! -f "${VERSIONING_CMAKE}" ]]; then
    echo "error: versioning.cmake not found at ${VERSIONING_CMAKE}"
    exit 1
fi

# Run versioning.cmake and generate versioning.h
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
cmake -S . -P "${VERSIONING_CMAKE}"

# Ensure versioning.h was generated
VERSIONING_HEADER="${PROJECT_DIR}/autogen/versioning.h"
if [ ! -f "${VERSIONING_HEADER}" ]; then
    echo "error: versioning.h was not generated!"
    exit 1
fi

# Extract variables from the generated header
RETROFE_VERSION_MAJOR=$(grep "RETROFE_VERSION_MAJOR" "${VERSIONING_HEADER}" | cut -d '"' -f 2)
RETROFE_VERSION_MINOR=$(grep "RETROFE_VERSION_MINOR" "${VERSIONING_HEADER}" | cut -d '"' -f 2)
RETROFE_VERSION_PATCH=$(grep "RETROFE_VERSION_PATCH" "${VERSIONING_HEADER}" | cut -d '"' -f 2)
RETROFE_VERSION_DESCRIBE=$(grep "RETROFE_VERSION_DESCRIBE" "${VERSIONING_HEADER}" | cut -d '"' -f 2)
RETROFE_BUILD_DATE=$(grep "RETROFE_BUILD_DATE" "${VERSIONING_HEADER}" | cut -d '"' -f 2)

# Validate that all variables were extracted
if [[ -z "${RETROFE_VERSION_MAJOR}" || -z "${RETROFE_VERSION_MINOR}" || -z "${RETROFE_VERSION_PATCH}" || -z "${RETROFE_VERSION_DESCRIBE}" || -z "${RETROFE_BUILD_DATE}" ]]; then
    echo "error: Failed to extract version information from versioning.h"
    exit 1
fi

# Update the Info.plist file
defaults write "${PLIST}" "CFBundleShortVersionString"   -string "${RETROFE_VERSION_DESCRIBE}"
defaults write "${PLIST}" "CFBundleVersion"              -string "${RETROFE_VERSION_DESCRIBE}-${RETROFE_VERSION_PATCH}"

# Output the updated values
echo "CFBundleShortVersionString:" "$(defaults read "${PLIST}" CFBundleShortVersionString)"
echo "CFBundleVersion:"            "$(defaults read "${PLIST}" CFBundleVersion)"