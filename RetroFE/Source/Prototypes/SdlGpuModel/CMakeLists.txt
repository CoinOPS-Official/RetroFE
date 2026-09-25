cmake_minimum_required(VERSION 3.24)
project(retrofe_sdl_gpu_model LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This first prototype uses DXIL shaders and currently targets Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GSTREAMER_ROOT "C:/gstreamer/1.0/msvc_x86_64" CACHE PATH "GStreamer MSVC x64 SDK")
find_package(SDL3 3.4 CONFIG REQUIRED)
find_package(SDL3_image 3.2 CONFIG REQUIRED)
find_program(DXC_EXECUTABLE dxc REQUIRED HINTS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64")

foreach(component gstreamer-1.0 gstapp-1.0 gstvideo-1.0 gobject-2.0 glib-2.0)
    string(REPLACE "-" "_" variable "GST_${component}")
    find_library(${variable} NAMES ${component} PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    list(APPEND GST_LIBRARIES "${${variable}}")
endforeach()

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader pbr.vert pbr.frag shadow.vert shadow.frag
               ao_geometry.vert ao_geometry.frag ao.frag post.vert post.frag)
    if(shader MATCHES "\\.vert$")
        set(profile vs_6_0)
    else()
        set(profile ps_6_0)
    endif()
    set(output "${SHADER_OUTPUT_DIR}/${shader}.dxil")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -T ${profile} -E main -Fo "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl"
        VERBATIM)
    list(APPEND SHADER_OUTPUTS "${output}")
endforeach()

add_executable(retrofe_sdl_gpu_model main.cpp ${SHADER_OUTPUTS})
target_compile_definitions(retrofe_sdl_gpu_model PRIVATE SDL_MAIN_HANDLED NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(retrofe_sdl_gpu_model PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/json/single_include")
target_link_libraries(retrofe_sdl_gpu_model PRIVATE SDL3::SDL3 SDL3_image::SDL3_image ${GST_LIBRARIES})
target_compile_options(retrofe_sdl_gpu_model PRIVATE /W4 /EHsc)
add_custom_command(TARGET retrofe_sdl_gpu_model POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:retrofe_sdl_gpu_model>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:retrofe_sdl_gpu_model>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:SDL3_image::SDL3_image>" "$<TARGET_FILE_DIR:retrofe_sdl_gpu_model>"
    VERBATIM)
