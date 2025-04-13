#!/bin/bash
# Xcode: Set version and build number from Git
# --------------------------------------------
#
# This script sets the version number `CFBundleVersion` to
# the number of Git commits up to the current `HEAD`.
# 
# Can also do commit short hash and commit details

set -e

function strip-v () { echo -n "${1#v}"; }
function strip-pre () { local x="${1#v}"; echo -n "${x%-*}"; }

PLIST="${PROJECT_DIR}/../xcode/Info.plist"

if [[ -d "${PROJECT_DIR}/../../.git" ]];
then

    COMMIT=$(git --git-dir="${PROJECT_DIR}/../../.git" rev-parse HEAD)

    TAG=$(strip-pre $(git describe --tags --match 'v[0-9]*' --abbrev=0 --exact-match 2> /dev/null || true))
    FULL_VERSION=$(strip-v $(git describe --tags --match 'v[0-9]*' --always ))
    BUILD=$(echo -n $(git rev-list HEAD | wc -l))
    
    defaults write "${PLIST}" "CFBundleVersion"            -string "${BUILD}"
    # defaults write "${PLIST}" "Commit"                     -string "${GIT_SHORT_HASH}"

else
    echo "warning: Building outside Git. Leaving version number untouched."
fi

echo "CFBundleIdentifier:"         "$(defaults read "${PLIST}" CFBundleIdentifier)"
echo "CFBundleShortVersionString:" "$(defaults read "${PLIST}" CFBundleShortVersionString)"
echo "CFBundleVersion:"            "$(defaults read "${PLIST}" CFBundleVersion)"