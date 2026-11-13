# What `cmake --install` puts in a prefix: the C library and what a program needs to use it
# (docs/integrating.md). The release archives and the packages are this install.
#
#   include/VASDK/c/vasdk.h, vasdk_providers.h, vasdk_c_export.h   the C API
#   include/VASDK/c/vasdk.hpp                                       the C API in C++, header-only
#   bin/vasdk                                                       the command: vasdk models pull ...
#   include/VASDK/version.h                                         VASDK_VERSION_*
#   lib/ (bin/ on Windows)   vasdk_c, and ONNX Runtime beside it when a provider uses it
#   lib/cmake/VASDK/         find_package(VASDK): the target VASDK::c
#   lib/pkgconfig/vasdk.pc   pkg-config --cflags --libs vasdk
#   share/doc/vasdk/         LICENSE, THIRD_PARTY_NOTICES.md
#
# Components: vasdk_runtime (the shared libraries) and vasdk_development (the rest). Never in a
# plain install: the sample apps (apps) and the C++ API (vasdk_cpp_development): the static C++
# libraries, their headers and, when their dependencies are installed packages, find_package(VASDK
# COMPONENTS cpp) with VASDK::VASDK, VASDK::providers and VASDK::<provider> (lib/cmake/VASDK/VASDKCpp*.cmake).

include(CMakePackageConfigHelpers)

set(VASDK_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/VASDK" CACHE STRING "Where find_package(VASDK)'s files are installed")
set(VASDK_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/vasdk")

# The providers built into the C library: find_package(VASDK COMPONENTS silero_vad whistle_stt)
# fails when one is missing
set(VASDK_BUILT_PROVIDERS "")
if(TARGET vasdk_provider_catalog)
    get_target_property(_vasdk_catalog_links vasdk_provider_catalog LINK_LIBRARIES)
    foreach(_link IN LISTS _vasdk_catalog_links)
        if(_link MATCHES "^vasdk_(.+)$" AND NOT _link STREQUAL "VASDK")
            list(APPEND VASDK_BUILT_PROVIDERS ${CMAKE_MATCH_1})
        endif()
    endforeach()
endif()

if(TARGET vasdk_c)
    install(TARGETS vasdk_c EXPORT VASDKTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT vasdk_runtime
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT vasdk_runtime NAMELINK_COMPONENT vasdk_development
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT vasdk_development
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:vasdk_c> DESTINATION ${CMAKE_INSTALL_BINDIR}
                COMPONENT vasdk_development OPTIONAL)
    endif()

    install(FILES
        ${VASDK_ROOT_DIR}/VASDK/include/VASDK/c/vasdk.h
        ${VASDK_ROOT_DIR}/VASDK/include/VASDK/c/vasdk_c_export.h
        ${VASDK_ROOT_DIR}/VASDK/include/VASDK/c/vasdk_providers.h
        ${VASDK_ROOT_DIR}/VASDK/include/VASDK/c/vasdk.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VASDK/c COMPONENT vasdk_development)
    install(FILES ${VASDK_ROOT_BINARY_DIR}/VASDK/include/VASDK/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VASDK COMPONENT vasdk_development)

    if(TARGET vasdk_command)
        install(TARGETS vasdk_command RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT vasdk_runtime)
    endif()

    # ONNX Runtime, which the C library loads: beside it (its rpath is $ORIGIN, @loader_path)
    if(VASDK_ONNXRUNTIME_RUNTIME_FILES)
        if(WIN32)
            set(_vasdk_runtime_dir ${CMAKE_INSTALL_BINDIR})
        else()
            set(_vasdk_runtime_dir ${CMAKE_INSTALL_LIBDIR})
        endif()
        install(FILES ${VASDK_ONNXRUNTIME_RUNTIME_FILES} DESTINATION ${_vasdk_runtime_dir} COMPONENT vasdk_runtime)
    endif()
    # Windows: every DLL it imports that the build knows of (OpenSSL, an installed protobuf, curl, abseil
    # from a package manager...), beside it in bin/. Elsewhere those are system libraries, or linked in when
    # fetched (VASDK_DEPENDENCY_SOURCE FETCH, what the release archives use). A package manager
    # turns this off: it ships those DLLs in their own packages.
    option(VASDK_INSTALL_RUNTIME_DEPENDENCIES "Windows: install the DLLs vasdk_c imports beside it" ON)
    if(WIN32 AND VASDK_INSTALL_RUNTIME_DEPENDENCIES)
        install(FILES $<TARGET_RUNTIME_DLLS:vasdk_c> ${VASDK_OPENSSL_RUNTIME_FILES} DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT vasdk_runtime)
    endif()

    # find_package(VASDK): VASDK::c
    install(EXPORT VASDKTargets
        NAMESPACE VASDK::
        DESTINATION ${VASDK_INSTALL_CMAKEDIR}
        COMPONENT vasdk_development)
    install(FILES ${VASDK_ROOT_DIR}/cmake/c_library/VASDKCopyRuntime.cmake
        DESTINATION ${VASDK_INSTALL_CMAKEDIR}
        COMPONENT vasdk_development)

    # The same package straight from this build tree, without installing: -DVASDK_DIR=<build>
    export(EXPORT VASDKTargets NAMESPACE VASDK:: FILE ${VASDK_ROOT_BINARY_DIR}/VASDKTargets.cmake)

    # pkg-config: paths relative to the .pc file, so the prefix can be moved
    file(RELATIVE_PATH _vasdk_pc_prefix
        "/vasdk_prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/vasdk_prefix")
    set(VASDK_PC_PREFIX "\${pcfiledir}/${_vasdk_pc_prefix}")
    string(REGEX REPLACE "/$" "" VASDK_PC_PREFIX "${VASDK_PC_PREFIX}")
    configure_file(${VASDK_ROOT_DIR}/cmake/vasdk.pc.in ${VASDK_ROOT_BINARY_DIR}/vasdk.pc @ONLY)
    install(FILES ${VASDK_ROOT_BINARY_DIR}/vasdk.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
        COMPONENT vasdk_development)

    install(FILES ${VASDK_ROOT_DIR}/LICENSE ${VASDK_ROOT_DIR}/THIRD_PARTY_NOTICES.md
        DESTINATION ${VASDK_INSTALL_DOCDIR}
        COMPONENT vasdk_runtime)
endif()

#  The C++ libraries as a package: find_package(VASDK COMPONENTS cpp), component vasdk_cpp_development
#
# VASDK, the provider catalog and each provider are static libraries, so a program linking one
# links what it links too: an exported target names every library of its link interface. Each must
# be another target of this package or one an installed package defines again in the program
# (find_dependency). A dependency this build fetched or compiles in (VASDK_DEPENDENCY_SOURCE FETCH,
# a provider's own sources) is neither, so the target linking it stays out of the package, and VASDK
# itself needs spdlog, nlohmann_json and protobuf installed (SYSTEM, or AUTO finding them).

# Imported targets by namespace, and the find_dependency() that defines them again in the program
set(_vasdk_cpp_known_packages
    "spdlog::"        "spdlog CONFIG"
    "fmt::"           "fmt CONFIG"
    "nlohmann_json::" "nlohmann_json CONFIG"
    "protobuf::"      "Protobuf CONFIG"
    "absl::"          "absl CONFIG"
    "httplib::"       "httplib CONFIG"
    "CURL::"          "CURL"
    "OpenSSL::"       "OpenSSL"
    "ZLIB::"          "ZLIB"
    "hwy::"           "hwy CONFIG"
    "onnxruntime::"   "onnxruntime CONFIG"
    "Threads::"       "Threads")

# _vasdk_cpp_check(<target> <exportable targets> <out reason> <out packages>): why <target> can't
# be exported (empty when it can), and the find_dependency() arguments its link interface needs
function(_vasdk_cpp_check _target _exportable _out_reason _out_packages)
    set(_reason "")
    set(_packages "")
    get_target_property(_links ${_target} INTERFACE_LINK_LIBRARIES)
    if(NOT _links)
        set(_links "")
    endif()
    foreach(_item IN LISTS _links)
        # $<LINK_ONLY:x> (a static library's private link) is exported as x, $<INSTALL_INTERFACE:x> into
        # the install's package and $<BUILD_INTERFACE:x> into the build tree's (export()): x either way.
        # The ::@ markers of a link made from another directory are not exported.
        while(_item MATCHES "^\\$<(LINK_ONLY|INSTALL_INTERFACE|BUILD_INTERFACE):(.*)>$")
            set(_item "${CMAKE_MATCH_2}")
        endwhile()
        if(_item STREQUAL "" OR _item MATCHES "^::@")
            continue()
        endif()
        if(_item MATCHES "^\\$<")
            set(_reason "links ${_item}, a condition the package does not check")
            break()
        endif()
        set(_real ${_item})
        set(_imported FALSE)
        if(TARGET ${_item})
            get_target_property(_aliased ${_item} ALIASED_TARGET)
            if(_aliased)
                set(_real ${_aliased})
            endif()
            get_target_property(_imported ${_real} IMPORTED)
        elseif(_item MATCHES "::")
            set(_imported TRUE)   # imported in another directory (CURL::libcurl in VASDK/): not visible here
        else()
            continue()   # a system library or a linker flag (winmm, dl, -framework CoreAudio)
        endif()
        if(_imported)
            set(_package "")
            list(LENGTH _vasdk_cpp_known_packages _count)
            math(EXPR _last "${_count} - 1")
            foreach(_index RANGE 0 ${_last} 2)
                list(GET _vasdk_cpp_known_packages ${_index} _namespace)
                string(FIND "${_real}" "${_namespace}" _at)
                if(_at EQUAL 0)
                    math(EXPR _package_index "${_index} + 1")
                    list(GET _vasdk_cpp_known_packages ${_package_index} _package)
                    break()
                endif()
            endforeach()
            if(_package STREQUAL "")
                set(_reason "links ${_real}, which no installed package defines")
                break()
            endif()
            list(APPEND _packages "${_package}")
        elseif(_real IN_LIST _vasdk_cpp_candidates AND NOT _real IN_LIST _exportable)
            set(_reason "links ${_real}, left out")
            break()
        elseif(NOT _real IN_LIST _exportable)
            set(_reason "links ${_real}, built here (fetched or compiled in), not installed")
            break()
        endif()
    endforeach()
    set(${_out_reason} "${_reason}" PARENT_SCOPE)
    set(${_out_packages} "${_packages}" PARENT_SCOPE)
endfunction()

# The candidates: VASDK, every provider of the catalog, the catalog. One that can't be exported
# takes out those that link it (the catalog links every provider), until nothing changes.
set(_vasdk_cpp_exportable VASDK)
if(TARGET vasdk_provider_catalog)
    get_target_property(_vasdk_catalog_links vasdk_provider_catalog LINK_LIBRARIES)
    foreach(_link IN LISTS _vasdk_catalog_links)
        if(TARGET ${_link} AND _link MATCHES "^vasdk_")
            list(APPEND _vasdk_cpp_exportable ${_link})
        endif()
    endforeach()
    list(APPEND _vasdk_cpp_exportable vasdk_provider_catalog)
endif()
set(_vasdk_cpp_candidates ${_vasdk_cpp_exportable})
set(_vasdk_cpp_left_out "")
set(_vasdk_cpp_changed TRUE)
while(_vasdk_cpp_changed)
    set(_vasdk_cpp_changed FALSE)
    foreach(_target IN LISTS _vasdk_cpp_exportable)
        _vasdk_cpp_check(${_target} "${_vasdk_cpp_exportable}" _reason _packages)
        if(_reason)
            list(REMOVE_ITEM _vasdk_cpp_exportable ${_target})
            list(APPEND _vasdk_cpp_left_out "${_target}: ${_reason}")
            set(_vasdk_cpp_changed TRUE)
        endif()
    endforeach()
endwhile()

set(VASDK_CPP_PACKAGE OFF)
if(NOT "VASDK" IN_LIST _vasdk_cpp_exportable)
    list(FILTER _vasdk_cpp_left_out INCLUDE REGEX "^VASDK: ")
    message(STATUS "VASDK: no find_package(VASDK COMPONENTS cpp) (${_vasdk_cpp_left_out}): the component "
                   "vasdk_cpp_development installs the C++ libraries and headers alone. The package needs the "
                   "dependencies installed: VASDK_DEPENDENCY_SOURCE SYSTEM or AUTO with spdlog, nlohmann_json, "
                   "protobuf, cpp-httplib, curl and highway from Conan or a package manager (docs/integrating.md)")
else()
    set(VASDK_CPP_PACKAGE ON)
    # The find_dependency() calls of what remains, and the providers in it
    set(_vasdk_cpp_dependencies "")
    set(VASDK_CPP_PROVIDERS "")
    foreach(_target IN LISTS _vasdk_cpp_exportable)
        _vasdk_cpp_check(${_target} "${_vasdk_cpp_exportable}" _reason _packages)
        list(APPEND _vasdk_cpp_dependencies ${_packages})
        if(NOT _target STREQUAL "VASDK" AND NOT _target STREQUAL "vasdk_provider_catalog")
            get_target_property(_export_name ${_target} EXPORT_NAME)
            list(APPEND VASDK_CPP_PROVIDERS ${_export_name})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _vasdk_cpp_dependencies)
    set(VASDK_CPP_FIND_DEPENDENCIES "")
    foreach(_dependency IN LISTS _vasdk_cpp_dependencies)
        string(APPEND VASDK_CPP_FIND_DEPENDENCIES "find_dependency(${_dependency})\n")
    endforeach()
    if("vasdk_provider_catalog" IN_LIST _vasdk_cpp_exportable)
        set(VASDK_CPP_HAS_CATALOG TRUE)
    else()
        set(VASDK_CPP_HAS_CATALOG FALSE)
    endif()

    install(TARGETS ${_vasdk_cpp_exportable} EXPORT VASDKCppTargets
        COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT VASDKCppTargets
        NAMESPACE VASDK::
        DESTINATION ${VASDK_INSTALL_CMAKEDIR}
        COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL)
    configure_file(${VASDK_ROOT_DIR}/cmake/VASDKCpp.cmake.in ${VASDK_ROOT_BINARY_DIR}/VASDKCpp.cmake @ONLY)
    install(FILES ${VASDK_ROOT_BINARY_DIR}/VASDKCpp.cmake
        DESTINATION ${VASDK_INSTALL_CMAKEDIR}
        COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL)
    export(EXPORT VASDKCppTargets NAMESPACE VASDK:: FILE ${VASDK_ROOT_BINARY_DIR}/VASDKCppTargets.cmake)

    list(JOIN VASDK_CPP_PROVIDERS ", " _vasdk_cpp_provider_text)
    set(_vasdk_cpp_message "VASDK: find_package(VASDK COMPONENTS cpp), installed with vasdk_cpp_development: VASDK::VASDK")
    if(VASDK_CPP_HAS_CATALOG)
        string(APPEND _vasdk_cpp_message ", VASDK::providers")
    endif()
    string(APPEND _vasdk_cpp_message ", providers '${_vasdk_cpp_provider_text}'")
    if(_vasdk_cpp_left_out)
        list(JOIN _vasdk_cpp_left_out "; " _vasdk_cpp_why)
        string(APPEND _vasdk_cpp_message ". Left out: ${_vasdk_cpp_why}")
    endif()
    message(STATUS "${_vasdk_cpp_message}")
endif()

#  find_package(VASDK): the C library (VASDK::c) and the C++ package (COMPONENTS cpp), whichever exists

if(TARGET vasdk_c OR VASDK_CPP_PACKAGE)
    configure_package_config_file(${VASDK_ROOT_DIR}/cmake/VASDKConfig.cmake.in
        ${VASDK_ROOT_BINARY_DIR}/VASDKConfig.cmake
        INSTALL_DESTINATION ${VASDK_INSTALL_CMAKEDIR})
    # A new minor release keeps the C API's binary interface, so any 1.x satisfies find_package(VASDK 1.2).
    # The C++ API makes no such promise: build it from the same source as the program.
    write_basic_package_version_file(${VASDK_ROOT_BINARY_DIR}/VASDKConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    # With the C library's development files, or with the C++ libraries when there is no C library (VASDK_SHARED)
    set(_vasdk_config_component vasdk_development)
    set(_vasdk_config_exclude "")
    if(NOT TARGET vasdk_c)
        set(_vasdk_config_component vasdk_cpp_development)
        set(_vasdk_config_exclude EXCLUDE_FROM_ALL)
    endif()
    install(FILES ${VASDK_ROOT_BINARY_DIR}/VASDKConfig.cmake ${VASDK_ROOT_BINARY_DIR}/VASDKConfigVersion.cmake
        DESTINATION ${VASDK_INSTALL_CMAKEDIR}
        COMPONENT ${_vasdk_config_component} ${_vasdk_config_exclude})
endif()
