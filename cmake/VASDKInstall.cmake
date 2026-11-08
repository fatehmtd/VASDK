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
# plain install: the sample apps (apps) and the C++ API (vasdk_cpp_development).

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
    # Windows: every DLL it imports that the build knows of (an installed protobuf, curl, abseil
    # from vcpkg...), beside it in bin/. Elsewhere those are system libraries, or linked in when
    # fetched (VASDK_DEPENDENCY_SOURCE FETCH, what the release archives use).
    if(WIN32)
        install(FILES $<TARGET_RUNTIME_DLLS:vasdk_c> DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT vasdk_runtime)
    endif()

    # find_package(VASDK)
    install(EXPORT VASDKTargets
        NAMESPACE VASDK::
        DESTINATION ${VASDK_INSTALL_CMAKEDIR}
        COMPONENT vasdk_development)
    configure_package_config_file(${VASDK_ROOT_DIR}/cmake/VASDKConfig.cmake.in
        ${VASDK_ROOT_BINARY_DIR}/VASDKConfig.cmake
        INSTALL_DESTINATION ${VASDK_INSTALL_CMAKEDIR})
    # A new minor release keeps the C API's binary interface, so any 1.x satisfies find_package(VASDK 1.2)
    write_basic_package_version_file(${VASDK_ROOT_BINARY_DIR}/VASDKConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES ${VASDK_ROOT_BINARY_DIR}/VASDKConfig.cmake ${VASDK_ROOT_BINARY_DIR}/VASDKConfigVersion.cmake
                  ${VASDK_ROOT_DIR}/cmake/c_library/VASDKCopyRuntime.cmake
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
