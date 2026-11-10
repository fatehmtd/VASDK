# vasdk_copy_runtime(<target>): Windows has no rpath, so a program finds vasdk_c.dll and the DLLs it
# loads (ONNX Runtime, an installed protobuf or curl) beside itself. After each build of <target>,
# this copies every DLL beside VASDK::c (the build tree's, or an install's bin/) next to <target>.
# Nothing to do elsewhere: the library's rpath finds them.
#
#   find_package(VASDK REQUIRED)                    # or FetchContent / add_subdirectory
#   target_link_libraries(my_app PRIVATE VASDK::c)
#   vasdk_copy_runtime(my_app)
#
# Defined by find_package(VASDK) and by a source build. Also run as a script (-P) by the copy itself.

if(CMAKE_SCRIPT_MODE_FILE)
    # The copy: every DLL in VASDK_COPY_FROM, and every file in VASDK_COPY_FILES, into VASDK_COPY_TO.
    # Programs in one folder copy the same DLLs as they finish linking, at the same moment, and
    # Windows refuses a file another copy is writing or replacing ("Permission denied"): the copies
    # take turns on VASDK_COPY_LOCK, a file of the build tree.
    if(VASDK_COPY_LOCK)
        file(LOCK "${VASDK_COPY_LOCK}" GUARD PROCESS TIMEOUT 600 RESULT_VARIABLE _locked)
        if(NOT _locked EQUAL 0)
            message(FATAL_ERROR "VASDK: can't lock ${VASDK_COPY_LOCK}: ${_locked}")
        endif()
    endif()
    set(_files ${VASDK_COPY_FILES})
    if(VASDK_COPY_FROM)
        file(GLOB _dlls "${VASDK_COPY_FROM}/*.dll")
        list(APPEND _files ${_dlls})
    endif()
    list(REMOVE_DUPLICATES _files)
    list(REMOVE_ITEM _files "")
    foreach(_file IN LISTS _files)
        get_filename_component(_directory "${_file}" DIRECTORY)
        get_filename_component(_name "${_file}" NAME)
        if(NOT _directory STREQUAL VASDK_COPY_TO)
            file(COPY_FILE "${_file}" "${VASDK_COPY_TO}/${_name}" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
    return()
endif()

set_property(GLOBAL PROPERTY VASDK_COPY_RUNTIME_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(vasdk_copy_runtime target)
    if(NOT WIN32)
        return()
    endif()
    get_property(_script GLOBAL PROPERTY VASDK_COPY_RUNTIME_SCRIPT)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DVASDK_COPY_FROM=$<TARGET_FILE_DIR:VASDK::c>"
                "-DVASDK_COPY_TO=$<TARGET_FILE_DIR:${target}>"
                "-DVASDK_COPY_LOCK=${CMAKE_BINARY_DIR}/CMakeFiles/vasdk_copy_runtime.lock" -P "${_script}"
        COMMENT "VASDK: vasdk_c.dll and the DLLs it loads beside ${target}"
        VERBATIM)
endfunction()
