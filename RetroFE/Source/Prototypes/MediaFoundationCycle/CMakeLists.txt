cmake_minimum_required(VERSION 3.24)
project(mediafoundation_cycle LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "This prototype requires Windows and the MSVC x64 SDKs")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GSTREAMER_ROOT "C:/gstreamer/1.0/msvc_x86_64" CACHE PATH "GStreamer MSVC x64 SDK")
set(FFMPEG_ROOT "E:/ffmpeg-9.0.2-full_build-shared" CACHE PATH "FFmpeg MSVC-compatible shared SDK")

find_package(SDL3 3.4 CONFIG REQUIRED)
find_path(GST_INCLUDE_DIR gst/gst.h PATHS "${GSTREAMER_ROOT}/include/gstreamer-1.0" NO_DEFAULT_PATH REQUIRED)
find_path(GLIB_INCLUDE_DIR glib.h PATHS "${GSTREAMER_ROOT}/include/glib-2.0" NO_DEFAULT_PATH REQUIRED)
find_path(GLIB_CONFIG_INCLUDE_DIR glibconfig.h PATHS "${GSTREAMER_ROOT}/lib/glib-2.0/include" NO_DEFAULT_PATH REQUIRED)
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

foreach(library gstreamer-1.0 gstapp-1.0 gstd3d11-1.0 gstvideo-1.0 gobject-2.0 glib-2.0)
    string(REPLACE "-" "_" variable "GST_${library}")
    string(REPLACE "." "_" variable "${variable}")
    find_library(${variable} NAMES "${library}" PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    list(APPEND GST_LIBRARIES "${${variable}}")
endforeach()
foreach(library avformat avcodec avutil)
    find_library(FF_${library} NAMES "${library}" PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    list(APPEND FF_LIBRARIES "${FF_${library}}")
endforeach()

add_executable(mediafoundation_cycle main.cpp)
target_compile_definitions(mediafoundation_cycle PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX GST_USE_UNSTABLE_API SDL_MAIN_HANDLED)
target_include_directories(mediafoundation_cycle PRIVATE
    "${GST_INCLUDE_DIR}" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include"
    "${GLIB_INCLUDE_DIR}" "${GLIB_CONFIG_INCLUDE_DIR}" "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(mediafoundation_cycle PRIVATE SDL3::SDL3 ${GST_LIBRARIES} ${FF_LIBRARIES}
    d3d11 dxgi mfplat mfreadwrite mfuuid ole32)
target_compile_options(mediafoundation_cycle PRIVATE /W4 /permissive- /EHsc)

add_custom_command(TARGET mediafoundation_cycle POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:mediafoundation_cycle>"
    VERBATIM)
