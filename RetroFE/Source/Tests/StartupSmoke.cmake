file(MAKE_DIRECTORY "${STARTUP_FIXTURE_DIR}")
file(WRITE "${STARTUP_FIXTURE_DIR}/settings.conf" "log=INFO\n")
file(REMOVE "${STARTUP_FIXTURE_DIR}/log.txt")

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
