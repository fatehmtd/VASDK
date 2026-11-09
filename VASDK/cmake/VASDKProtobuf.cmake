# Provides protobuf::libprotobuf and protobuf_generate(): an installed protobuf (CONFIG package:
# Conan, Homebrew, a distro package) when there is one, otherwise built from source.
#
# Where protoc finds Google's own .proto files (google/protobuf/any.proto, struct.proto, ...): an
# installed protoc finds them in its include/ folder, one built from source has none, so they come
# from protobuf's sources. Two global variables, empty for an installed protobuf, which every protoc
# call passes: VASDK_PROTOBUF_IMPORT_DIRS to protobuf_generate(IMPORT_DIRS), VASDK_PROTOC_IMPORT_ARGUMENTS
# (-I <folder>) to a protoc command of its own.
set(VASDK_PROTOBUF_IMPORT_DIRS "" CACHE INTERNAL "")
set(VASDK_PROTOC_IMPORT_ARGUMENTS "" CACHE INTERNAL "")
if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf CONFIG QUIET)
endif()

if(TARGET protobuf::libprotobuf)
    # Imported targets are directory-scoped; 3P provider options are generated from other directories.
    foreach(_target protobuf::libprotobuf protobuf::protoc)
        get_target_property(_imported ${_target} IMPORTED)
        if(_imported)
            set_target_properties(${_target} PROPERTIES IMPORTED_GLOBAL TRUE)
        endif()
    endforeach()
    message(STATUS "VASDK: using installed protobuf ${Protobuf_VERSION}")
elseif(VASDK_DEPENDENCY_SOURCE STREQUAL "SYSTEM")
    message(FATAL_ERROR "VASDK: protobuf not found and VASDK_DEPENDENCY_SOURCE is SYSTEM: "
                        "install it (Conan, your package manager) or use FETCH or AUTO")
else()
    message(STATUS "VASDK: protobuf not installed - building it from source (first configure is slow)")
    include(FetchContent)
    set(VASDK_PROTOBUF_VERSION 36.2)
    if(CMAKE_CROSSCOMPILING)
        # Code is generated on the build machine: a protoc built for the target (Android) can't
        # run here. Use the build machine's protoc of the same version, else download it.
        set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
        find_program(VASDK_HOST_PROTOC protoc NO_CMAKE_FIND_ROOT_PATH)
        if(VASDK_HOST_PROTOC)
            execute_process(COMMAND ${VASDK_HOST_PROTOC} --version OUTPUT_VARIABLE _protoc_version OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _protoc_version STREQUAL "libprotoc ${VASDK_PROTOBUF_VERSION}")
                message(STATUS "VASDK: ${VASDK_HOST_PROTOC} is ${_protoc_version}, not ${VASDK_PROTOBUF_VERSION}: downloading protoc")
                unset(VASDK_HOST_PROTOC CACHE)
                set(VASDK_HOST_PROTOC "")
            endif()
        endif()
        if(NOT VASDK_HOST_PROTOC)
            if(CMAKE_HOST_APPLE)
                set(_protoc_platform osx-universal_binary)
            elseif(CMAKE_HOST_WIN32)
                set(_protoc_platform win64)
            elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
                set(_protoc_platform linux-aarch_64)
            else()
                set(_protoc_platform linux-x86_64)
            endif()
            fetchcontent_declare(protoc_host
                URL https://github.com/protocolbuffers/protobuf/releases/download/v${VASDK_PROTOBUF_VERSION}/protoc-${VASDK_PROTOBUF_VERSION}-${_protoc_platform}.zip)
            fetchcontent_makeavailable(protoc_host)
            set(VASDK_HOST_PROTOC ${protoc_host_SOURCE_DIR}/bin/protoc${CMAKE_HOST_EXECUTABLE_SUFFIX})
        endif()
        add_executable(protobuf::protoc IMPORTED GLOBAL)
        set_target_properties(protobuf::protoc PROPERTIES IMPORTED_LOCATION ${VASDK_HOST_PROTOC})
        message(STATUS "VASDK: generating code with ${VASDK_HOST_PROTOC}")
    endif()
    set(protobuf_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL            OFF CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB          OFF CACHE BOOL "" FORCE)
    set(ABSL_PROPAGATE_CXX_STD      ON  CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL         OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG        v${VASDK_PROTOBUF_VERSION}
        GIT_SHALLOW    TRUE
        ${VASDK_FETCH_EXCLUDE}   # its own install rules stay out of VASDK's
    )
    fetchcontent_makeavailable(protobuf)
    include(${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake)
    set(VASDK_PROTOBUF_IMPORT_DIRS ${protobuf_SOURCE_DIR}/src CACHE INTERNAL "")
    set(VASDK_PROTOC_IMPORT_ARGUMENTS -I ${protobuf_SOURCE_DIR}/src CACHE INTERNAL "")
endif()
