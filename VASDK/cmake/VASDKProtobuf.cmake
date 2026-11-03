# Provides protobuf::libprotobuf and protobuf_generate(): an installed protobuf (CONFIG
# package, e.g. Homebrew or vcpkg) when there is one, otherwise built from source.
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
else()
    message(STATUS "VASDK: protobuf not installed - building it from source (first configure is slow)")
    include(FetchContent)
    set(protobuf_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL            OFF CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB          OFF CACHE BOOL "" FORCE)
    set(ABSL_PROPAGATE_CXX_STD      ON  CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL         OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG        v36.2
        GIT_SHALLOW    TRUE
    )
    fetchcontent_makeavailable(protobuf)
    include(${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake)
endif()
