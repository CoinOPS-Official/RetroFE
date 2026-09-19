function(retrofe_stage_runtime target)
    if(NOT WIN32)
        return()
    endif()
    # Plugins required by playback, visualizers, filters and hardware decoding.
    set(gst_plugins app audioconvert audioparsers audioresample coreelements
        d3d11 d3d12 qsv isomp4 libav matroska playback typefindfunctions
        videoconvertscale videofilter videoparsersbad geometrictransform
        goom audiovisualizers avi wavparse ogg vorbis flac)
    set(runtime_dlls)
    foreach(plugin IN LISTS gst_plugins)
        set(path "${GSTREAMER_ROOT}/lib/gstreamer-1.0/gst${plugin}.dll")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "Required GStreamer plugin missing: ${path}")
        endif()
        list(APPEND runtime_dlls "${path}")
    endforeach()
    foreach(library gstapp gstaudio gstpbutils gstvideo gthread)
        if(library STREQUAL "gthread")
            list(APPEND runtime_dlls "${GSTREAMER_ROOT}/bin/gthread-2.0-0.dll")
        else()
            list(APPEND runtime_dlls "${GSTREAMER_ROOT}/bin/${library}-1.0-0.dll")
        endif()
    endforeach()
    set(runtime_dirs "${GSTREAMER_ROOT}/bin")
    set(runtime_licenses)
    foreach(package SDL3 SDL3_image SDL3_ttf SDL3_mixer)
        get_target_property(location ${package}::${package} IMPORTED_LOCATION)
        if(location)
            get_filename_component(dll_dir "${location}" DIRECTORY)
            # SDL codec DLLs can be loaded dynamically rather than imported.
            file(GLOB codec_dlls "${dll_dir}/*.dll" "${dll_dir}/optional/*.dll")
            list(APPEND runtime_dlls ${codec_dlls})
            list(APPEND runtime_dirs "${dll_dir}" "${dll_dir}/optional")
        endif()
        # Official VC archives keep redistribution notices at their root and
        # beside optional codec DLLs. Retain relative paths to avoid collisions.
        if(DEFINED ${package}_DIR)
            get_filename_component(package_root "${${package}_DIR}/.." ABSOLUTE)
            file(GLOB_RECURSE notices LIST_DIRECTORIES FALSE
                "${package_root}/LICENSE*" "${package_root}/COPYING*")
            foreach(notice IN LISTS notices)
                file(RELATIVE_PATH relative "${package_root}" "${notice}")
                list(APPEND runtime_licenses "${notice}|licenses/${package}/${relative}")
            endforeach()
        endif()
    endforeach()
    file(GLOB other_dlls "${CURL_ROOT}/bin/*.dll" "${ZLIB_ROOT}/*.dll"
        "${CURL_ROOT}/tools/curl/zlib1.dll"
        "${LIBUSB_ROOT}/dll/*.dll" "${LIBSERIALPORT_ROOT}/bin/*.dll"
        "${LIBSERIALPORT_ROOT}/*.dll")
    list(APPEND runtime_dlls ${other_dlls})
    foreach(dll IN LISTS other_dlls)
        get_filename_component(dir "${dll}" DIRECTORY)
        list(APPEND runtime_dirs "${dir}")
    endforeach()
    set(scanner "${GSTREAMER_ROOT}/libexec/gstreamer-1.0/gst-plugin-scanner.exe")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PruneRuntime.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PruneRuntime.cmake" @ONLY)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DOUTPUT_DIR=$<TARGET_FILE_DIR:${target}>"
            "-DREMOVE_LEGACY_TESTS=$<STREQUAL:${target},retrofe>"
            "-DEXTRA_FILES=$<TARGET_RUNTIME_DLLS:${target}>"
            -P "${CMAKE_CURRENT_BINARY_DIR}/PruneRuntime.cmake"
        VERBATIM)
endfunction()
