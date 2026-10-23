cmake_minimum_required(VERSION 3.24)

file(MAKE_DIRECTORY "${STARTUP_FIXTURE_DIR}")
file(WRITE "${STARTUP_FIXTURE_DIR}/settings.conf"
    "log=INFO\ndumpProperties=true\nrootSequence=base\nrootOrder=base\n")
foreach(number IN ITEMS 1 2 10 15 16)
    file(WRITE "${STARTUP_FIXTURE_DIR}/settings${number}.conf" "rootSequence=${number}\n")
endforeach()
file(WRITE "${STARTUP_FIXTURE_DIR}/settings_saved.conf" "rootOrder=saved\n")

if(WIN32)
    set(config_os windows)
elseif(APPLE)
    set(config_os apple)
else()
    set(config_os linux)
endif()
set(collection_dir "${STARTUP_FIXTURE_DIR}/collections/Scope")
file(MAKE_DIRECTORY "${collection_dir}/launchers.${config_os}.local" "${collection_dir}/launchers.local")
file(WRITE "${collection_dir}/settings.conf"
    "sequence=base\noverride=base\nlauncher=explicit\nitem=%ITEM_COLLECTION_NAME%\n")
foreach(number IN ITEMS 1 2 10 15 16)
    file(WRITE "${collection_dir}/settings${number}.conf" "sequence=${number}\noverride=${number}\n")
endforeach()
file(WRITE "${collection_dir}/info.conf" "override=info\n")
file(WRITE "${collection_dir}/launcher.conf" "executable=default\n")
file(WRITE "${collection_dir}/launcher.${config_os}.conf" "executable=os\n")
file(WRITE "${collection_dir}/launchers.local/choice.conf" "executable=default-local\n")
file(WRITE "${collection_dir}/launchers.${config_os}.local/choice.conf" "executable=os-local\n")

set(fallback_dir "${STARTUP_FIXTURE_DIR}/collections/Fallback")
file(MAKE_DIRECTORY "${fallback_dir}/launchers.local")
file(WRITE "${fallback_dir}/settings.conf" "sequence=fallback\n")
file(WRITE "${fallback_dir}/launcher.conf" "executable=default\n")
file(WRITE "${fallback_dir}/launchers.${config_os}.local" "not a directory\n")
file(WRITE "${fallback_dir}/launchers.local/choice.conf" "executable=fallback-local\n")
file(MAKE_DIRECTORY "${STARTUP_FIXTURE_DIR}/collections/_Hidden")
file(WRITE "${STARTUP_FIXTURE_DIR}/collections/_Hidden/settings.conf" "hidden=yes\n")
file(REMOVE "${STARTUP_FIXTURE_DIR}/log.txt")
file(REMOVE "${STARTUP_FIXTURE_DIR}/properties.txt")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "RETROFE_PATH=${STARTUP_FIXTURE_DIR}"
        "${RETROFE_EXECUTABLE}" -showconfig
    RESULT_VARIABLE result
    TIMEOUT 10)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "RetroFE configuration startup failed: ${result}")
endif()

if(NOT EXISTS "${STARTUP_FIXTURE_DIR}/log.txt")
    message(FATAL_ERROR "RetroFE did not write its startup log")
endif()
file(READ "${STARTUP_FIXTURE_DIR}/log.txt" startup_log)
if(NOT startup_log MATCHES "Absolute path:")
    message(FATAL_ERROR "RetroFE exited during initial configuration import:\n${startup_log}")
endif()
if(startup_log MATCHES "Failed to initialize GStreamer")
    message(FATAL_ERROR "RetroFE rejected lazy GStreamer initialization:\n${startup_log}")
endif()

if(NOT EXISTS "${STARTUP_FIXTURE_DIR}/properties.txt")
    message(FATAL_ERROR "Configuration import did not finish and dump its properties")
endif()
file(STRINGS "${STARTUP_FIXTURE_DIR}/properties.txt" properties)
foreach(expected IN ITEMS "rootSequence=15" "rootOrder=saved"
    "collections.Scope.sequence=15" "collections.Scope.override=info"
    "collections.Scope.launcher=explicit" "collections.Scope.item=Scope"
    "collectionLaunchers.Scope.executable=os" "localLaunchers.Scope.choice.executable=os-local"
    "collections.Fallback.launcher=Fallback" "collectionLaunchers.Fallback.executable=default"
    "localLaunchers.Fallback.choice.executable=fallback-local")
    if(NOT expected IN_LIST properties)
        message(FATAL_ERROR "Configuration precedence or launcher fallback changed: missing ${expected}")
    endif()
endforeach()
foreach(property IN LISTS properties)
    if(property MATCHES "^collections\\._Hidden\\.")
        message(FATAL_ERROR "Hidden collection was unexpectedly imported: ${property}")
    endif()
endforeach()
