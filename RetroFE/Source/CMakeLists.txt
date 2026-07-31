cmake_minimum_required(VERSION 3.12)

# Disable Visual Studio's user-wide vcpkg integration before project()
# configures the compiler and creates any targets, including subdirectories.
list(APPEND CMAKE_VS_GLOBALS
    "VcpkgEnabled=false"
    "VcpkgApplocalDeps=false"
)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project (retrofe)

set(CMAKE_CXX_STANDARD 20) # Upgraded from 17 to support coroutines
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(RETROFE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(RETROFE_THIRD_PARTY_DIR "${RETROFE_DIR}/ThirdParty")
list(APPEND CMAKE_MODULE_PATH "${RETROFE_DIR}/CMake")

set(OPENHI2TXT_BUILD_CLI OFF CACHE BOOL "Build the OpenHi2txt command-line executable" FORCE)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
add_subdirectory(
    "${RETROFE_THIRD_PARTY_DIR}/openhi2txt"
    "${CMAKE_BINARY_DIR}/openhi2txt"
)
if(MSVC)
    set_target_properties(openhi2txt PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()

##############################################################
# Versioning
# Ensure versioning.h is generated before compiling
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/autogen/versioning.h"
    COMMAND ${CMAKE_COMMAND} -P "${RETROFE_DIR}/CMake/Versioning.cmake"
    COMMENT "Generating versioning.h"
)
# Create a custom target to generate versioning.h
add_custom_target(GenerateVersioningHeader ALL DEPENDS "${CMAKE_BINARY_DIR}/autogen/versioning.h")
include_directories("${CMAKE_BINARY_DIR}/autogen")

##############################################################
# Setup some variables to help find external libraries
##############################################################

set(SQLITE3_ROOT "${RETROFE_THIRD_PARTY_DIR}/sqlite3")
set(QRCODEGEN_ROOT "${RETROFE_THIRD_PARTY_DIR}/qrcodegen")
set(CURL_ROOT "${RETROFE_THIRD_PARTY_DIR}/curl_x64-windows")
set(JSON_ROOT "${RETROFE_THIRD_PARTY_DIR}/json/single_include")
set(RAPIDXML_ROOT "${RETROFE_THIRD_PARTY_DIR}/rapidxml-1.17")
set(KISSFFT_ROOT "${RETROFE_THIRD_PARTY_DIR}/kissfft-131.1.0")

# Keep the Windows SDL2 SDK and runtime versions in one place. These are the
# latest native SDL2 releases, not the separate sdl2-compat project.
set(RETROFE_SDL2_VERSION "2.32.10")
set(RETROFE_SDL2_IMAGE_VERSION "2.8.12")
set(RETROFE_SDL2_MIXER_VERSION "2.8.2")
set(RETROFE_SDL2_TTF_VERSION "2.24.0")

if(WIN32)
    if(MSVC)
        set(SDL2_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2-${RETROFE_SDL2_VERSION}")
        set(SDL2_IMAGE_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_image-${RETROFE_SDL2_IMAGE_VERSION}")
        set(SDL2_MIXER_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_mixer-${RETROFE_SDL2_MIXER_VERSION}")
        set(SDL2_TTF_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_ttf-${RETROFE_SDL2_TTF_VERSION}")
    endif()

    set(ZLIB_ROOT "${RETROFE_THIRD_PARTY_DIR}/zlib13-dll")
    set(GSTREAMER_ROOT "C:/gstreamer/1.0/msvc_x64" CACHE STRING "location of where your gstreamer include and lib folders reside")
    set(GLIB2_ROOT "${GSTREAMER_ROOT}")

    # Set libusb paths
    set(LIBUSB_ROOT "${RETROFE_THIRD_PARTY_DIR}/libusb-1.0.30")
    include_directories("${LIBUSB_ROOT}/include")

    set(LIBSERIALPORT_ROOT "${RETROFE_THIRD_PARTY_DIR}/libserialport")
    include_directories("${LIBSERIALPORT_ROOT}")

endif()

if(WIN32)
    find_package(Glib2 REQUIRED)
    find_package(GStreamer REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(SDL2_mixer REQUIRED)
    find_package(SDL2_ttf REQUIRED)
    find_package(ZLIB REQUIRED)

elseif(APPLE)
    find_package(Glib2 REQUIRED)
    find_package(GStreamer REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(SDL2_mixer REQUIRED)
    find_package(SDL2_ttf REQUIRED)
    find_package(ZLIB REQUIRED)
    find_library(COREFOUNDATION CoreFoundation)
    find_library(IOKIT IOKit)
    pkg_check_modules(GLIB2 REQUIRED glib-2.0 gobject-2.0 gthread-2.0)
    pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-video-1.0 gstreamer-plugins-base-1.0)

    # Manually specify paths if needed
    set(LIBUSB_INCLUDE_DIR "/opt/homebrew/include" CACHE PATH "Path to libusb include directory")
    set(LIBUSB_LIBRARY "/opt/homebrew/lib/libusb-1.0.dylib" CACHE FILEPATH "Path to libusb library")
    
else()
    include(FindPkgConfig)
    
    # Find necessary libraries using pkg-config
    pkg_search_module(SDL2 REQUIRED sdl2)
    pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image)
    pkg_search_module(SDL2_MIXER REQUIRED SDL2_mixer)
    pkg_search_module(SDL2_TTF REQUIRED SDL2_ttf)
    pkg_search_module(ZLIB REQUIRED zlib)
    pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-app-1.0)
    pkg_check_modules(Glib2 REQUIRED glib-2.0 gobject-2.0 gthread-2.0 gmodule-2.0)
    pkg_search_module(LIBUSB REQUIRED libusb-1.0)
    pkg_check_modules(LIBEVDEV REQUIRED libevdev)
    pkg_check_modules(LIBUDEV  REQUIRED libudev)
    pkg_search_module(LIBSERIALPORT REQUIRED libserialport)
    pkg_search_module(LIBCURL REQUIRED libcurl)
    find_package(Threads REQUIRED)
    
    # Include directories for all found libraries
    include_directories(
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_IMAGE_INCLUDE_DIRS}
        ${SDL2_MIXER_INCLUDE_DIRS}
        ${SDL2_TTF_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
        ${GSTREAMER_INCLUDE_DIRS}
        ${GLIB2_INCLUDE_DIRS}
        ${Threads_INCLUDE_DIRS}
        ${LIBUSB_INCLUDE_DIRS}
        ${LIBUSB_INCLUDE_DIR}
        ${LIBEVDEV_INCLUDE_DIRS}
        ${LIBUDEV_INCLUDE_DIRS}
        ${LIBSERIALPORT_INCLUDE_DIRS}
        ${LIBCURL_INCLUDE_DIRS}
    )
    
    # Link directories (optional, mostly handled by pkg-config)
    link_directories(
        ${SDL2_LIBRARY_DIRS}
        ${SDL2_IMAGE_LIBRARY_DIRS}
        ${SDL2_MIXER_LIBRARY_DIRS}
        ${SDL2_TTF_LIBRARY_DIRS}
        ${ZLIB_LIBRARY_DIRS}
        ${GSTREAMER_LIBRARY_DIRS}
        ${GLIB2_LIBRARY_DIRS}
        ${LIBUSB_LIBRARY_DIRS}
        ${LIBEVDEV_LIBRARY_DIRS}
        ${LIBUDEV_LIBRARY_DIRS}
        ${LIBSERIALPORT_LIBRARY_DIRS}
        ${LIBCURL_LIBRARY_DIRS}
    )
    
    # Append the necessary libraries to be linked
    list(APPEND RETROFE_LIBRARIES
        ${SDL2_LIBRARIES}
        ${SDL2_IMAGE_LIBRARIES}
        ${SDL2_MIXER_LIBRARIES}
        ${SDL2_TTF_LIBRARIES}
        ${ZLIB_LIBRARIES}
        ${GSTREAMER_LIBRARIES}
        ${GLIB2_LIBRARIES}
        ${CMAKE_DL_LIBS}
        ${CMAKE_THREAD_LIBS_INIT}
        ${LIBUSB_LIBRARIES}
        ${LIBEVDEV_LIBRARIES}
        ${LIBUDEV_LIBRARIES} 
        ${LIBSERIALPORT_LIBRARIES}
        ${LIBCURL_LIBRARIES}
    )
    add_definitions(${LIBEVDEV_CFLAGS_OTHER})
    endif()

set(RETROFE_INCLUDE_DIRS
    "${GLIB2_INCLUDE_DIRS}"
    "${GSTREAMER_INCLUDE_DIRS}"
    "${SDL2_INCLUDE_DIRS}"
    "${SDL2_IMAGE_INCLUDE_DIRS}"
    "${SDL2_MIXER_INCLUDE_DIRS}"
    "${SDL2_TTF_INCLUDE_DIRS}"
    "${ZLIB_INCLUDE_DIRS}"
    "${QRCODEGEN_ROOT}"
    "${JSON_ROOT}"
    "${CURL_ROOT}/include"
    "${SQLITE3_ROOT}"
    "${KISSFFT_ROOT}"
    "/opt/homebrew/lib/glib-2.0/include"
)

if(WIN32)
    list(APPEND RETROFE_INCLUDE_DIRS "${LIBUSB_ROOT}/include")
endif()

# Link libusb library if on Windows
if(WIN32)
    set(RETROFE_LIBRARIES
    ${GLIB2_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${ZLIB_LIBRARIES}
)
    list(APPEND RETROFE_LIBRARIES "${LIBUSB_ROOT}/dll/libusb-1.0.lib")
    list(APPEND RETROFE_LIBRARIES 
        "${LIBSERIALPORT_ROOT}/lib/libserialport.lib"
        "dxgi.lib"
        "${CURL_ROOT}/lib/libcurl.lib"
    )
endif()

if(NOT WIN32)
    LIST(APPEND RETROFE_LIBRARIES ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
endif()

set(RETROFE_HEADERS
	"${RETROFE_DIR}/Source/Collection/CollectionInfo.h"
	"${RETROFE_DIR}/Source/Collection/CollectionInfoBuilder.h"
	"${RETROFE_DIR}/Source/Collection/Item.h"
	"${RETROFE_DIR}/Source/Collection/MenuParser.h"
    "${RETROFE_DIR}/Source/Collection/PlaylistDirtyRegistry.h"
    "${RETROFE_DIR}/Source/Collection/PlaylistPath.h"
	"${RETROFE_DIR}/Source/Control/UserInput.h"
	"${RETROFE_DIR}/Source/Control/InputHandler.h"
	"${RETROFE_DIR}/Source/Control/GameControllerAxisHandler.h"
	"${RETROFE_DIR}/Source/Control/GameControllerButtonHandler.h"
	"${RETROFE_DIR}/Source/Control/JoyAxisHandler.h"
	"${RETROFE_DIR}/Source/Control/JoyButtonHandler.h"
	"${RETROFE_DIR}/Source/Control/JoyHatHandler.h"
	"${RETROFE_DIR}/Source/Control/KeyboardHandler.h"
	"${RETROFE_DIR}/Source/Control/MouseButtonHandler.h"
	"${RETROFE_DIR}/Source/Control/MouseAxisHandler.h"
	"${RETROFE_DIR}/Source/Control/TouchDragHandler.h"
    "${RETROFE_DIR}/Source/Control/Restrictor/Restrictor.h"
    "${RETROFE_DIR}/Source/Control/Restrictor/RestrictorManager.h"
    "${RETROFE_DIR}/Source/Control/Restrictor/ServoStikRestrictor.h"
    "${RETROFE_DIR}/Source/Control/Restrictor/TOSGRSRestrictor.h"
	"${RETROFE_DIR}/Source/Database/Configuration.h"
	"${RETROFE_DIR}/Source/Database/DB.h"
	"${RETROFE_DIR}/Source/Database/GlobalOpts.h"
    "${RETROFE_DIR}/Source/Database/GlobalHiScores.h"
    "${RETROFE_DIR}/Source/Database/HighScoreView.h"
    "${RETROFE_DIR}/Source/Database/LocalHiScores.h"
    "${RETROFE_DIR}/Source/Execute/AttractMode.h"
    "${RETROFE_DIR}/Source/Execute/Input/InputMonitor.h"
    "${RETROFE_DIR}/Source/Execute/Input/SDLJoyStickScopeGuard.h"
    "${RETROFE_DIR}/Source/Execute/Input/IKeyboardBackend.h"
    "${RETROFE_DIR}/Source/Execute/Input/KeyboardBackendFactory.h"
    "${RETROFE_DIR}/Source/Execute/Launcher.h"
    "${RETROFE_DIR}/Source/Execute/PatchExecutor.h"
    "${RETROFE_DIR}/Source/Execute/Platform/IProcessManager.h"
    "${RETROFE_DIR}/Source/Execute/Platform/Windows/WindowsProcessManager.h"
    "${RETROFE_DIR}/Source/Execute/Platform/Unix/UnixProcessManager.h"
    "${RETROFE_DIR}/Source/Execute/Util/RestrictorGuard.h"
	"${RETROFE_DIR}/Source/Graphics/Animate/Tween.h"
	"${RETROFE_DIR}/Source/Graphics/Animate/TweenTypes.h"
	"${RETROFE_DIR}/Source/Graphics/Animate/TweenSet.h"
	"${RETROFE_DIR}/Source/Graphics/Animate/Animation.h"
	"${RETROFE_DIR}/Source/Graphics/Animate/AnimationEvents.h"
	"${RETROFE_DIR}/Source/Graphics/ComponentItemBinding.h"
	"${RETROFE_DIR}/Source/Graphics/Component/Container.h"
	"${RETROFE_DIR}/Source/Graphics/Component/Component.h"
	"${RETROFE_DIR}/Source/Graphics/Component/Image.h"
	"${RETROFE_DIR}/Source/Graphics/Component/ImageBuilder.h"
	"${RETROFE_DIR}/Source/Graphics/Component/MusicPlayerComponent.h"
    "${RETROFE_DIR}/Source/Graphics/Component/ReloadableHiscores.h"
    "${RETROFE_DIR}/Source/Graphics/Component/LocalScorePagePlan.h"
    "${RETROFE_DIR}/Source/Graphics/Component/ReloadableGlobalHiscores.h"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableMedia.h"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableText.h"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableScrollingText.h"
	"${RETROFE_DIR}/Source/Graphics/Component/ScrollingList.h"
	"${RETROFE_DIR}/Source/Graphics/Component/Text.h"
	"${RETROFE_DIR}/Source/Graphics/Component/VideoComponent.h"
	"${RETROFE_DIR}/Source/Graphics/Component/VideoBuilder.h"
	"${RETROFE_DIR}/Source/Graphics/Font.h"
	"${RETROFE_DIR}/Source/Graphics/FontCache.h"
	"${RETROFE_DIR}/Source/Graphics/PageBuilder.h"
	"${RETROFE_DIR}/Source/Graphics/Page.h"
	"${RETROFE_DIR}/Source/Menu/Menu.h"
	"${RETROFE_DIR}/Source/Sound/AudioBus.h"
	"${RETROFE_DIR}/Source/Sound/Sound.h"
    "${RETROFE_DIR}/Source/Sound/MusicPlayer.h"
    "${RETROFE_DIR}/Source/Sound/VuMeterDSP.h"
	"${RETROFE_DIR}/Source/Utility/Log.h"
    "${RETROFE_DIR}/Source/Utility/PayloadSync.h"
	"${RETROFE_DIR}/Source/Utility/ThreadPool.h"
	"${RETROFE_DIR}/Source/Utility/Utils.h"
    "${RETROFE_DIR}/Source/Utility/CrashHandler.h"
	"${RETROFE_DIR}/Source/Video/IVideo.h"
    "${RETROFE_DIR}/Source/Video/GlibLoop.h"
	"${RETROFE_DIR}/Source/Video/GStreamerVideo.h"
	"${RETROFE_DIR}/Source/Video/VideoFactory.h"
    "${RETROFE_DIR}/Source/Video/VideoPool.h"
   	"${RETROFE_DIR}/Source/Graphics/ComponentItemBindingBuilder.h"
	"${RETROFE_DIR}/Source/Graphics/ViewInfo.h"
	"${RETROFE_DIR}/Source/RetroFE.h"
	"${RETROFE_DIR}/Source/SDL.h"
    "${SQLITE3_ROOT}/sqlite3.h"
)

set(RETROFE_SOURCES
	"${RETROFE_DIR}/Source/Collection/CollectionInfo.cpp"
	"${RETROFE_DIR}/Source/Collection/CollectionInfoBuilder.cpp"
	"${RETROFE_DIR}/Source/Collection/Item.cpp"
    "${RETROFE_DIR}/Source/Collection/PlaylistDirtyRegistry.cpp"
	"${RETROFE_DIR}/Source/Collection/MenuParser.cpp"
	"${RETROFE_DIR}/Source/Control/UserInput.cpp"
	"${RETROFE_DIR}/Source/Control/GameControllerAxisHandler.cpp"
	"${RETROFE_DIR}/Source/Control/GameControllerButtonHandler.cpp"
	"${RETROFE_DIR}/Source/Control/JoyAxisHandler.cpp"
	"${RETROFE_DIR}/Source/Control/JoyButtonHandler.cpp"
	"${RETROFE_DIR}/Source/Control/JoyHatHandler.cpp"
	"${RETROFE_DIR}/Source/Control/KeyboardHandler.cpp"
	"${RETROFE_DIR}/Source/Control/MouseButtonHandler.cpp"
    "${RETROFE_DIR}/Source/Control/MouseAxisHandler.cpp"
	"${RETROFE_DIR}/Source/Control/TouchDragHandler.cpp"
    "${RETROFE_DIR}/Source/Control/Restrictor/Restrictor.cpp"
    "${RETROFE_DIR}/Source/Control/Restrictor/RestrictorManager.cpp"
    "${RETROFE_DIR}/Source/Control/Restrictor/ServoStikRestrictor.cpp"
    "${RETROFE_DIR}/Source/Control/Restrictor/TOSGRSRestrictor.cpp"
	"${RETROFE_DIR}/Source/Database/Configuration.cpp"
	"${RETROFE_DIR}/Source/Database/DB.cpp"
	"${RETROFE_DIR}/Source/Database/GlobalOpts.cpp"
    "${RETROFE_DIR}/Source/Database/GlobalHiScores.cpp"
    "${RETROFE_DIR}/Source/Database/LocalHiScores.cpp"
	"${RETROFE_DIR}/Source/Database/MetadataDatabase.cpp"
    "${RETROFE_DIR}/Source/Execute/AttractMode.cpp"
    "${RETROFE_DIR}/Source/Execute/Input/InputMonitor.cpp"
    "${RETROFE_DIR}/Source/Execute/Input/Platform/KeyboardBackendLinux.cpp"
    "${RETROFE_DIR}/Source/Execute/Input/Platform/KeyboardBackendWindows.cpp"
    "${RETROFE_DIR}/Source/Execute/Input/Platform/KeyboardBackendMac.cpp"
    "${RETROFE_DIR}/Source/Execute/Launcher.cpp"
    "${RETROFE_DIR}/Source/Execute/PatchExecutor.cpp"
    "${RETROFE_DIR}/Source/Execute/Platform/Windows/WindowsProcessManager.cpp"
    "${RETROFE_DIR}/Source/Execute/Platform/Unix/UnixProcessManager.cpp"
	"${RETROFE_DIR}/Source/Graphics/Font.cpp"
	"${RETROFE_DIR}/Source/Graphics/FontCache.cpp"
	"${RETROFE_DIR}/Source/Graphics/PageBuilder.cpp"
	"${RETROFE_DIR}/Source/Graphics/Page.cpp"
	"${RETROFE_DIR}/Source/Graphics/ViewInfo.cpp"
	"${RETROFE_DIR}/Source/Graphics/Animate/Animation.cpp"
	"${RETROFE_DIR}/Source/Graphics/Animate/AnimationEvents.cpp"
	"${RETROFE_DIR}/Source/Graphics/Animate/Tween.cpp"
	"${RETROFE_DIR}/Source/Graphics/Animate/TweenSet.cpp"
	"${RETROFE_DIR}/Source/Graphics/ComponentItemBindingBuilder.cpp"
	"${RETROFE_DIR}/Source/Graphics/ComponentItemBinding.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/Container.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/Component.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/Image.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ImageBuilder.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/MusicPlayerComponent.cpp"
    "${RETROFE_DIR}/Source/Graphics/Component/ReloadableHiscores.cpp"
    "${RETROFE_DIR}/Source/Graphics/Component/LocalScorePagePlan.cpp"
    "${RETROFE_DIR}/Source/Graphics/Component/ReloadableGlobalHiscores.cpp"
  	"${RETROFE_DIR}/Source/Graphics/Component/Text.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableMedia.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableText.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableScrollingText.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ScrollingList.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/VideoBuilder.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/VideoComponent.cpp"
	"${RETROFE_DIR}/Source/Menu/Menu.cpp"
	"${RETROFE_DIR}/Source/Sound/AudioBus.cpp"
	"${RETROFE_DIR}/Source/Sound/Sound.cpp"
    "${RETROFE_DIR}/Source/Sound/MusicPlayer.cpp"
    "${RETROFE_DIR}/Source/Sound/VuMeterDSP.cpp"
	"${RETROFE_DIR}/Source/Utility/Log.cpp"
    "${RETROFE_DIR}/Source/Utility/PayloadSync.cpp"
	"${RETROFE_DIR}/Source/Utility/ThreadPool.cpp"
	"${RETROFE_DIR}/Source/Utility/Utils.cpp"
    "${RETROFE_DIR}/Source/Utility/CrashHandler.cpp"
    "${RETROFE_DIR}/Source/Video/GlibLoop.cpp"
	"${RETROFE_DIR}/Source/Video/GStreamerVideo.cpp"
	"${RETROFE_DIR}/Source/Video/VideoFactory.cpp"
    "${RETROFE_DIR}/Source/Video/VideoPool.cpp"
	"${RETROFE_DIR}/Source/Main.cpp"
	"${RETROFE_DIR}/Source/RetroFE.cpp"
	"${RETROFE_DIR}/Source/SDL.cpp"
    "${QRCODEGEN_ROOT}/qrcodegen.cpp"
    "${SQLITE3_ROOT}/sqlite3.c"
    "${KISSFFT_ROOT}/kiss_fft.c"
    "${KISSFFT_ROOT}/kiss_fftr.c"
)

set(EXECUTABLE_OUTPUT_PATH "${RETROFE_DIR}/Build" CACHE PATH "Build directory" FORCE)
set(LIBRARY_OUTPUT_PATH "${RETROFE_DIR}/Build" CACHE PATH "Build directory" FORCE)

include_directories(${RETROFE_INCLUDE_DIRS})
include_directories(SYSTEM "${RAPIDXML_ROOT}")
set_source_files_properties("${SQLITE3_ROOT}/sqlite3.c" PROPERTIES COMPILE_FLAGS "-w")
add_executable(retrofe  ${RETROFE_SOURCES} ${RETROFE_HEADERS})
add_dependencies(retrofe GenerateVersioningHeader)

target_link_libraries(retrofe ${RETROFE_LIBRARIES})
target_link_libraries(retrofe openhi2txt::openhi2txt)

if(APPLE)
    target_link_libraries(retrofe
        ${COREFOUNDATION}
        ${IOKIT}
        ${SDL2_LIBRARIES}
        ${SDL2_IMAGE_LIBRARIES}
        ${SDL2_MIXER_LIBRARIES}
        ${SDL2_TTF_LIBRARIES}
        ${LIBUSB_LIBRARY}
        ${GLIB2_LIBRARIES}
        ${GSTREAMER_LIBRARIES}
        ${GSTREAMER_VIDEO_LIBRARIES}
    )
    
    # Set up macOS-specific properties
    set_target_properties(retrofe PROPERTIES OUTPUT_NAME "retrofe")
    set_target_properties(retrofe PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(MSVC)
    set(CMAKE_DEBUG_POSTFIX "d")
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
    
    # Enable C++20 Strict Conformance so the __cplusplus macro reports correctly
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus /permissive-")

    # Base flags: Multi-core build, dynamic runtime
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /WX- /MD")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP /WX- /MD")

    # Aggressive Release Optimizations
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /GL /Gw /Gy")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /Oi /GL /Gw /Gy")
    
    # Linker flags to support dead-code stripping and Link-Time Code Generation
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")

    set_target_properties(retrofe PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")

    # Package the same SDL DLLs that supplied the headers and import libraries.
    # This overwrites any older copies supplied by the Windows runtime bundle.
    foreach(SDL_RUNTIME_DLL IN ITEMS
        "${SDL2_ROOT}/lib/x64/SDL2.dll"
        "${SDL2_IMAGE_ROOT}/lib/x64/SDL2_image.dll"
        "${SDL2_MIXER_ROOT}/lib/x64/SDL2_mixer.dll"
        "${SDL2_TTF_ROOT}/lib/x64/SDL2_ttf.dll"
    )
        add_custom_command(TARGET retrofe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:retrofe>"
            VERBATIM
        )
    endforeach()
else()
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -Wall -ggdb")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -Wall -ggdb")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set_target_properties(retrofe PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO / LTO is not supported by this compiler.")
    endif()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        # -O3 is generally safe and preferable for media frontends over -O2
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -march=x86-64-v2 -msse4.2 -mavx -Wall -Wextra -Wno-deprecated-declarations -funroll-loops")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=x86-64-v2 -msse4.2 -mavx -Wall -Wextra -Wno-reorder -Wno-deprecated-declarations -funroll-loops")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        # Removed -ffast-math to protect Tween interpolation logic
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=native -funroll-loops -fstrict-aliasing -Wall -Wextra -Wno-deprecated-declarations")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=native -funroll-loops -fstrict-aliasing -Wall -Wextra -Wno-reorder -Wno-deprecated-declarations")
    endif()
    
    message(STATUS "Using C Flags: ${CMAKE_C_FLAGS}")
    message(STATUS "Using CXX Flags: ${CMAKE_CXX_FLAGS}")
endif()
