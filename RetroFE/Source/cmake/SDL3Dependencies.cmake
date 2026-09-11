include(FetchContent)
option(RETROFE_FETCH_SDL3 "Fetch pinned SDL3 dependencies when config packages are absent" ON)

# Installed config packages take precedence. Explicit FETCHCONTENT_SOURCE_DIR_*
# overrides also support offline source trees without writing into those trees.
set(SDL_SHARED ON CACHE BOOL "Build shared SDL")
set(SDL_STATIC OFF CACHE BOOL "Build static SDL")
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL test library")
set(SDL_TESTS OFF CACHE BOOL "Build SDL tests")
set(SDLIMAGE_VENDORED ON CACHE BOOL "Use pinned image codec submodules")
# AVIF artwork support is optional; avoid building its dav1d/aom codecs by
# default. Video decoding is handled separately by GStreamer.
set(SDLIMAGE_AVIF OFF CACHE BOOL "Enable AVIF artwork support")
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "Build image samples")
set(SDLIMAGE_TESTS OFF CACHE BOOL "Build image tests")
set(SDLTTF_VENDORED ON CACHE BOOL "Use pinned font submodules")
set(SDLTTF_SAMPLES OFF CACHE BOOL "Build font samples")
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared dependencies")

macro(retrofe_sdl_dependency package version repository commit)
    find_package(${package} ${version} CONFIG QUIET)
    if(NOT TARGET ${package}::${package})
        if(NOT RETROFE_FETCH_SDL3)
            message(FATAL_ERROR "Missing ${package} config package. Set CMAKE_PREFIX_PATH or enable RETROFE_FETCH_SDL3.")
        endif()
        FetchContent_Declare(${package}
            GIT_REPOSITORY https://github.com/libsdl-org/${repository}.git
            GIT_TAG ${commit}
            GIT_SUBMODULES_RECURSE TRUE)
        FetchContent_MakeAvailable(${package})
    endif()
endmacro()

retrofe_sdl_dependency(SDL3 3.4.12 SDL f87239e71e42da91ca317a12eefb82cfbf3393eb)
retrofe_sdl_dependency(SDL3_image 3.2.4 SDL_image 11154afb7855293159588b245b446a4ef09e574f)
retrofe_sdl_dependency(SDL3_ttf 3.2.2 SDL_ttf a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b)

set(SDLMIXER_VENDORED ON CACHE BOOL "Use pinned mixer codecs")
set(SDLMIXER_SAMPLES OFF CACHE BOOL "Build mixer samples")
set(SDLMIXER_TESTS OFF CACHE BOOL "Build mixer tests")
retrofe_sdl_dependency(SDL3_mixer 3.2.4 SDL_mixer 72a81869b45e249e8e67102db4e98dd2441f05a1)
