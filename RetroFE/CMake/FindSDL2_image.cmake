# - Find SDL2_image library and headers
# 
# Find module for SDL_image 2.0 (http://www.libsdl.org/projects/SDL_image/).
# It defines the following variables:
#  SDL2_IMAGE_INCLUDE_DIRS - The location of the headers, e.g., SDL_image.h.
#  SDL2_IMAGE_LIBRARIES - The libraries to link against to use SDL2_image.
#  SDL2_IMAGE_FOUND - If false, do not try to use SDL2_image.
#  SDL2_IMAGE_VERSION_STRING
#    Human-readable string containing the version of SDL2_image.
#
# Also defined, but not for general use are:
#   SDL2_IMAGE_INCLUDE_DIR - The directory that contains SDL_image.h.
#   SDL2_IMAGE_LIBRARY - The location of the SDL2_image library.
#

#=============================================================================
# Copyright 2013 Benjamin Eikel
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# (To distribute this file outside of CMake, substitute the full
#  License text for the above reference.)

find_package(PkgConfig QUIET)
pkg_check_modules(PC_SDL2_IMAGE QUIET SDL2_image)

# A version-pinned SDK directory may have been replaced since the last
# configure. Discard missing cached results so find_path/find_library rerun.
if(SDL2_IMAGE_INCLUDE_DIR AND NOT EXISTS "${SDL2_IMAGE_INCLUDE_DIR}/SDL_image.h")
  unset(SDL2_IMAGE_INCLUDE_DIR CACHE)
endif()
if(SDL2_IMAGE_LIBRARY AND NOT EXISTS "${SDL2_IMAGE_LIBRARY}")
  unset(SDL2_IMAGE_LIBRARY CACHE)
endif()

find_path(SDL2_IMAGE_INCLUDE_DIR
  NAMES SDL_image.h
  HINTS
    ${SDL2_IMAGE_ROOT}/include
  PATH_SUFFIXES SDL2
)

find_library(SDL2_IMAGE_LIBRARY
  NAMES SDL2_image
  HINTS
    ${SDL2_IMAGE_ROOT}/lib
  PATH_SUFFIXES x64
)

if(SDL2_IMAGE_INCLUDE_DIR AND EXISTS "${SDL2_IMAGE_INCLUDE_DIR}/SDL_image.h")
  file(STRINGS "${SDL2_IMAGE_INCLUDE_DIR}/SDL_image.h" SDL2_IMAGE_VERSION_MAJOR_LINE REGEX "^#define[ \t]+SDL_IMAGE_MAJOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL2_IMAGE_INCLUDE_DIR}/SDL_image.h" SDL2_IMAGE_VERSION_MINOR_LINE REGEX "^#define[ \t]+SDL_IMAGE_MINOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL2_IMAGE_INCLUDE_DIR}/SDL_image.h" SDL2_IMAGE_VERSION_PATCH_LINE REGEX "^#define[ \t]+SDL_IMAGE_PATCHLEVEL[ \t]+[0-9]+$")
  string(REGEX REPLACE "^#define[ \t]+SDL_IMAGE_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL2_IMAGE_VERSION_MAJOR "${SDL2_IMAGE_VERSION_MAJOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_IMAGE_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL2_IMAGE_VERSION_MINOR "${SDL2_IMAGE_VERSION_MINOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_IMAGE_PATCHLEVEL[ \t]+([0-9]+)$" "\\1" SDL2_IMAGE_VERSION_PATCH "${SDL2_IMAGE_VERSION_PATCH_LINE}")
  set(SDL2_IMAGE_VERSION_STRING ${SDL2_IMAGE_VERSION_MAJOR}.${SDL2_IMAGE_VERSION_MINOR}.${SDL2_IMAGE_VERSION_PATCH})
  unset(SDL2_IMAGE_VERSION_MAJOR_LINE)
  unset(SDL2_IMAGE_VERSION_MINOR_LINE)
  unset(SDL2_IMAGE_VERSION_PATCH_LINE)
  unset(SDL2_IMAGE_VERSION_MAJOR)
  unset(SDL2_IMAGE_VERSION_MINOR)
  unset(SDL2_IMAGE_VERSION_PATCH)
endif()

set(SDL2_IMAGE_INCLUDE_DIRS ${SDL2_IMAGE_INCLUDE_DIR})
set(SDL2_IMAGE_LIBRARIES ${SDL2_IMAGE_LIBRARY})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(SDL2_image
                                  REQUIRED_VARS SDL2_IMAGE_INCLUDE_DIRS SDL2_IMAGE_LIBRARIES
                                  VERSION_VAR SDL2_IMAGE_VERSION_STRING)

mark_as_advanced(SDL2_IMAGE_INCLUDE_DIR SDL2_IMAGE_LIBRARY)
