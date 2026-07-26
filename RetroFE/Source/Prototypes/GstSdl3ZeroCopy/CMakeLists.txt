cmake_minimum_required(VERSION 3.24)

project(gst_sdl3_nv12_copy LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This prototype currently targets the SDL3 Direct3D 11 renderer on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(
    GSTREAMER_ROOT
    "C:/gstreamer/1.0/msvc_x86_64"
    CACHE PATH
    "GStreamer MSVC x64 installation root"
)

find_path(
    GSTREAMER_INCLUDE_DIR
    gst/gst.h
    PATHS "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    NO_DEFAULT_PATH
    REQUIRED
)
find_path(
    GLIB_INCLUDE_DIR
    glib.h
    PATHS "${GSTREAMER_ROOT}/include/glib-2.0"
    NO_DEFAULT_PATH
    REQUIRED
)
find_path(
    GLIB_CONFIG_INCLUDE_DIR
    glibconfig.h
    PATHS "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    NO_DEFAULT_PATH
    REQUIRED
)

function(find_gstreamer_import_library output_name library_name)
    find_library(
        ${output_name}
        NAMES ${library_name}
        PATHS "${GSTREAMER_ROOT}/lib"
        NO_DEFAULT_PATH
        REQUIRED
    )
    set(${output_name} "${${output_name}}" PARENT_SCOPE)
endfunction()

find_gstreamer_import_library(GSTREAMER_LIBRARY gstreamer-1.0)
find_gstreamer_import_library(GSTAPP_LIBRARY gstapp-1.0)
find_gstreamer_import_library(GSTVIDEO_LIBRARY gstvideo-1.0)
find_gstreamer_import_library(GSTD3D11_LIBRARY gstd3d11-1.0)
find_gstreamer_import_library(GOBJECT_LIBRARY gobject-2.0)
find_gstreamer_import_library(GLIB_LIBRARY glib-2.0)

option(
    GST_SDL3_NV12_COPY_USE_SYSTEM_SDL3
    "Use an already installed SDL3 CMake package instead of the pinned source"
    OFF
)

if(GST_SDL3_NV12_COPY_USE_SYSTEM_SDL3)
    find_package(SDL3 3.4 CONFIG REQUIRED)
else()
    include(FetchContent)

    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.12
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

add_executable(gst_sdl3_nv12_copy main.cpp)

target_compile_definitions(
    gst_sdl3_nv12_copy
    PRIVATE
        GST_USE_UNSTABLE_API
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        SDL_MAIN_HANDLED
)

target_include_directories(
    gst_sdl3_nv12_copy
    PRIVATE
        "${GSTREAMER_INCLUDE_DIR}"
        "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include"
        "${GLIB_INCLUDE_DIR}"
        "${GLIB_CONFIG_INCLUDE_DIR}"
)

target_link_libraries(
    gst_sdl3_nv12_copy
    PRIVATE
        SDL3::SDL3
        "${GSTREAMER_LIBRARY}"
        "${GSTAPP_LIBRARY}"
        "${GSTVIDEO_LIBRARY}"
        "${GSTD3D11_LIBRARY}"
        "${GOBJECT_LIBRARY}"
        "${GLIB_LIBRARY}"
        d3d11
        dxgi
)

if(MSVC)
    target_compile_options(gst_sdl3_nv12_copy PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(gst_sdl3_nv12_copy PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(
    TARGET gst_sdl3_nv12_copy
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:gst_sdl3_nv12_copy>"
    VERBATIM
)
