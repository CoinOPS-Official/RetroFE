cmake_minimum_required(VERSION 3.10)

project (retrofe)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# set(CMAKE_FIND_FRAMEWORK LAST)
set(RETROFE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(RETROFE_THIRD_PARTY_DIR "${RETROFE_DIR}/ThirdParty")
list(APPEND CMAKE_MODULE_PATH "${RETROFE_DIR}/CMake")

# Find Git executable
find_package(Git REQUIRED)

# Get the Git hash
execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_NO
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

##############################################################
# Setup some variables to help find external libraries
##############################################################

set(SQLITE3_ROOT "${RETROFE_THIRD_PARTY_DIR}/sqlite3")
set(RAPIDXML_ROOT "${RETROFE_THIRD_PARTY_DIR}/rapidxml-1.13")

if(WIN32)
if(MSVC)
	set(SDL2_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2-2.30.0")
 	set(SDL2_IMAGE_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_image-2.8.2")
 	set(SDL2_MIXER_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_mixer-2.6.3")
 	set(SDL2_TTF_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_ttf-2.20.2")
elseif(MINGW)
 	set(SDL2_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2-2.30.0-mingw/x86_64-w64-mingw32")
 	set(SDL2_IMAGE_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_image-2.8.2-mingw/x86_64-w64-mingw32")
	set(SDL2_MIXER_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_mixer-2.6.3-mingw/x86_64-w64-mingw32")
	set(SDL2_TTF_ROOT "${RETROFE_THIRD_PARTY_DIR}/SDL2_ttf-2.20.2-mingw/x86_64-w64-mingw32")
endif()

	set(ZLIB_ROOT "${RETROFE_THIRD_PARTY_DIR}/zlib13-dll")

	set(GSTREAMER_ROOT "C:/gstreamer/1.0/msvc_x64" CACHE STRING "location of where your gstreamer include and lib folders reside")
	set(GLIB2_ROOT "${GSTREAMER_ROOT}")
  
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
	
else()
	include(FindPkgConfig)
	pkg_search_module(SDL2 REQUIRED sdl2)
	pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image)
	pkg_search_module(SDL2_MIXER REQUIRED SDL2_mixer)
	pkg_search_module(SDL2_TTF REQUIRED SDL2_ttf)
	pkg_search_module(ZLIB REQUIRED zlib)
	pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-app-1.0)
	pkg_check_modules(Glib2 REQUIRED glib-2.0 gobject-2.0 gthread-2.0 gmodule-2.0)
	find_package(Threads REQUIRED)
endif()

set(RETROFE_INCLUDE_DIRS
	"${GLIB2_INCLUDE_DIRS}"
	"${GSTREAMER_INCLUDE_DIRS}"
	"${SDL2_INCLUDE_DIRS}"
	"${SDL2_IMAGE_INCLUDE_DIRS}"
	"${SDL2_MIXER_INCLUDE_DIRS}"
	"${SDL2_TTF_INCLUDE_DIRS}"
	"${ZLIB_INCLUDE_DIRS}"
	"${SQLITE3_ROOT}"
	"${RAPIDXML_ROOT}"
	"/opt/homebrew/lib/glib-2.0/include"
)

set(RETROFE_LIBRARIES
	${GLIB2_LIBRARIES}
	${GSTREAMER_LIBRARIES}
	${SDL2_LIBRARIES}
	${SDL2_IMAGE_LIBRARIES}
	${SDL2_MIXER_LIBRARIES}
	${SDL2_TTF_LIBRARIES}
	${ZLIB_LIBRARIES}
)

if(NOT WIN32)
	LIST(APPEND RETROFE_LIBRARIES ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
endif()

set(RETROFE_HEADERS
	"${RETROFE_DIR}/Source/Collection/CollectionInfo.h"
	"${RETROFE_DIR}/Source/Collection/CollectionInfoBuilder.h"
	"${RETROFE_DIR}/Source/Collection/Item.h"
	"${RETROFE_DIR}/Source/Collection/MenuParser.h"
	"${RETROFE_DIR}/Source/Control/UserInput.h"
	"${RETROFE_DIR}/Source/Control/InputHandler.h"
	"${RETROFE_DIR}/Source/Control/JoyAxisHandler.h"
	"${RETROFE_DIR}/Source/Control/JoyButtonHandler.h"
	"${RETROFE_DIR}/Source/Control/JoyHatHandler.h"
	"${RETROFE_DIR}/Source/Control/KeyboardHandler.h"
	"${RETROFE_DIR}/Source/Control/MouseButtonHandler.h"
	"${RETROFE_DIR}/Source/Database/Configuration.h"
	"${RETROFE_DIR}/Source/Database/DB.h"
	"${RETROFE_DIR}/Source/Database/GlobalOpts.h"
	"${RETROFE_DIR}/Source/Database/MetadataDatabase.h"
	"${RETROFE_DIR}/Source/Execute/AttractMode.h"
	"${RETROFE_DIR}/Source/Execute/Launcher.h"
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
	"${RETROFE_DIR}/Source/Graphics/ThreadPool.h"
	"${RETROFE_DIR}/Source/Menu/Menu.h"
	"${RETROFE_DIR}/Source/Sound/Sound.h"
	"${RETROFE_DIR}/Source/Utility/Log.h"
	"${RETROFE_DIR}/Source/Utility/Utils.h"
	"${RETROFE_DIR}/Source/Video/IVideo.h"
	"${RETROFE_DIR}/Source/Video/GStreamerVideo.h"
	"${RETROFE_DIR}/Source/Video/VideoFactory.h"
	"${RETROFE_DIR}/Source/Graphics/ComponentItemBindingBuilder.h"
	"${RETROFE_DIR}/Source/Graphics/ViewInfo.h"
	"${RETROFE_DIR}/Source/RetroFE.h"
	"${RETROFE_DIR}/Source/SDL.h"
	"${RETROFE_DIR}/Source/Version.h"
)

set(RETROFE_SOURCES
	"${RETROFE_DIR}/Source/Collection/CollectionInfo.cpp"
	"${RETROFE_DIR}/Source/Collection/CollectionInfoBuilder.cpp"
	"${RETROFE_DIR}/Source/Collection/Item.cpp"
	"${RETROFE_DIR}/Source/Collection/MenuParser.cpp"
	"${RETROFE_DIR}/Source/Control/UserInput.cpp"
	"${RETROFE_DIR}/Source/Control/JoyAxisHandler.cpp"
	"${RETROFE_DIR}/Source/Control/JoyButtonHandler.cpp"
	"${RETROFE_DIR}/Source/Control/JoyHatHandler.cpp"
	"${RETROFE_DIR}/Source/Control/KeyboardHandler.cpp"
	"${RETROFE_DIR}/Source/Control/MouseButtonHandler.cpp"
	"${RETROFE_DIR}/Source/Database/Configuration.cpp"
	"${RETROFE_DIR}/Source/Database/DB.cpp"
	"${RETROFE_DIR}/Source/Database/GlobalOpts.cpp"
	"${RETROFE_DIR}/Source/Database/MetadataDatabase.cpp"
	"${RETROFE_DIR}/Source/Execute/AttractMode.cpp"
	"${RETROFE_DIR}/Source/Execute/Launcher.cpp"
	"${RETROFE_DIR}/Source/Graphics/Font.cpp"
	"${RETROFE_DIR}/Source/Graphics/FontCache.cpp"
	"${RETROFE_DIR}/Source/Graphics/PageBuilder.cpp"
	"${RETROFE_DIR}/Source/Graphics/Page.cpp"
	"${RETROFE_DIR}/Source/Graphics/ThreadPool.cpp"
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
	"${RETROFE_DIR}/Source/Graphics/Component/Text.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableMedia.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableText.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ReloadableScrollingText.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/ScrollingList.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/VideoBuilder.cpp"
	"${RETROFE_DIR}/Source/Graphics/Component/VideoComponent.cpp"
	"${RETROFE_DIR}/Source/Menu/Menu.cpp"
	"${RETROFE_DIR}/Source/Sound/Sound.cpp"
	"${RETROFE_DIR}/Source/Utility/Log.cpp"
	"${RETROFE_DIR}/Source/Utility/Utils.cpp"
	"${RETROFE_DIR}/Source/Video/GStreamerVideo.cpp"
	"${RETROFE_DIR}/Source/Video/VideoFactory.cpp"
	"${RETROFE_DIR}/Source/Main.cpp"
	"${RETROFE_DIR}/Source/RetroFE.cpp"
	"${RETROFE_DIR}/Source/SDL.cpp"
	"${RETROFE_DIR}/Source/Version.cpp"
	"${SQLITE3_ROOT}/sqlite3.c"
)


set(EXECUTABLE_OUTPUT_PATH "${RETROFE_DIR}/Build" CACHE PATH "Build directory" FORCE)
set(LIBRARY_OUTPUT_PATH "${RETROFE_DIR}/Build" CACHE PATH "Build directory" FORCE)


include_directories(${RETROFE_INCLUDE_DIRS})
add_executable(retrofe  ${RETROFE_SOURCES} ${RETROFE_HEADERS})

if(NOT APPLE)
	target_link_libraries(retrofe ${RETROFE_LIBRARIES})
endif()

if(APPLE)
	# Get the current dir then go to package contents
	get_filename_component(DIR_ABOVE ../../Package/Environment/Common ABSOLUTE)
	# set as var
	set(RETROFE_PACKAGE_DIR ${DIR_ABOVE})

	# Set the output directory for the executable inside the app bundle
	set_target_properties(retrofe PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_BUNDLE_NAME "RetroFE"
		MACOSX_BUNDLE_BUNDLE_VERSION "0.10.34.3"
		MACOSX_BUNDLE_COPYRIGHT "CoinOPS Project 2023, ported to Apple Silicon by aidenjbass, Program originally by the RetroFE team"
		MACOSX_BUNDLE_ICON_FILE "RetroFE.png"
	)
	# Copy necessary resources into the app bundle
		# List of resource files to be copied into the bundle
	set(APP_RESOURCES
		${RETROFE_PACKAGE_DIR}/RetroFE.png
	)
	# Create folder
	add_custom_command(TARGET retrofe POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:retrofe>/../Resources 
	)
	foreach(RESOURCE ${APP_RESOURCES})
		message("COPYING RESOURCE ${RESOURCE} TO BUNDLE")
		add_custom_command(TARGET retrofe POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy ${RESOURCE} $<TARGET_FILE_DIR:retrofe>/../Resources
		)
	endforeach()

	target_link_libraries(retrofe ${RETROFE_LIBRARIES})

else()
	set_target_properties(retrofe PROPERTIES OUTPUT_NAME "retrofe-${GIT_COMMIT_NO}")
	set_target_properties(retrofe PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(MINGW)
  set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -lmingw32 -mwindows")
endif()

add_definitions(-DRETROFE_VERSION_MAJOR=${VERSION_MAJOR})
add_definitions(-DRETROFE_VERSION_MINOR=${VERSION_MINOR})
add_definitions(-DRETROFE_VERSION_BUILD=${VERSION_BUILD})

if(MSVC)
	set(CMAKE_DEBUG_POSTFIX "d")
	add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
	add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /WX /MT")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP /WX /MT")

	set_target_properties(retrofe PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
else()
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wall -ggdb")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall -ggdb")
  set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ffast-math -Wall -funroll-loops")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ffast-math -Wall -Wno-reorder -funroll-loops")
endif()