cmake_minimum_required(VERSION 3.12)

find_package(Git)
if(GIT_FOUND)
    # Get the Git commit numerically i.e. "199"
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_NO
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Get the Git short hash i.e. "b5c50ca"
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_SHORT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Get the Git branch name
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} 
        OUTPUT_VARIABLE RETROFE_WC_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

string(TIMESTAMP RETROFE_BUILD_DATE "%Y-%m-%d" UTC)

# version number
set(RETROFE_VERSION_MAJOR "2504")
set(RETROFE_VERSION_MINOR "2")
set(RETROFE_VERSION_PATCH ${GIT_SHORT_HASH})
set(RETROFE_VERSION_GITNO ${GIT_COMMIT_NO})

# If RETROFE_WC_BRANCH is master
if(RETROFE_WC_BRANCH STREQUAL "master")
    set(RETROFE_WC_BRANCH "")
endif()

# If RetroFE is not built from a repo, default the version info
if(NOT GIT_FOUND)
    set(RETROFE_VERSION_PATCH 0)
    set(RETROFE_VERSION_GITNO "0")
    set(RETROFE_WC_BRANCH "Outside Git")
endif()

set(RETROFE_VERSION_DESCRIBE "${RETROFE_VERSION_MAJOR}.${RETROFE_VERSION_MINOR}")

# Generate versioning.h
set(VERSION_HEADER "${CMAKE_BINARY_DIR}/autogen/versioning.h")
file(WRITE ${VERSION_HEADER} "// Auto-generated versioning header\n")
file(APPEND ${VERSION_HEADER} "#ifndef VERSIONING_H\n#define VERSIONING_H\n\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_VERSION_MAJOR \"${RETROFE_VERSION_MAJOR}\"\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_VERSION_MINOR \"${RETROFE_VERSION_MINOR}\"\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_VERSION_PATCH \"${RETROFE_VERSION_PATCH}\"\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_VERSION_GITNO \"${RETROFE_VERSION_GITNO}\"\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_VERSION_DESCRIBE \"${RETROFE_VERSION_DESCRIBE}\"\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_WC_BRANCH \"${RETROFE_WC_BRANCH}\"\n")
file(APPEND ${VERSION_HEADER} "#define RETROFE_BUILD_DATE \"${RETROFE_BUILD_DATE}\"\n\n")
file(APPEND ${VERSION_HEADER} "#endif // VERSIONING_H\n")
