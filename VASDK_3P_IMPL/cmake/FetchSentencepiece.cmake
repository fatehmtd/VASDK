# SentencePiece (PocketTTS, PhononTTS, MossTTS share it), static.
if(NOT TARGET sentencepiece-static)
    include(FetchContent)
    set(SPM_ENABLE_SHARED   OFF CACHE BOOL "" FORCE)
    set(SPM_BUILD_TEST      OFF CACHE BOOL "" FORCE)
    set(SPM_BUILD_EXAMPLE   OFF CACHE BOOL "" FORCE)
    set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
    set(SPM_PROTOBUF_PROVIDER "internal" CACHE STRING "" FORCE)
    # sentencepiece v0.2.0 has cmake_minimum_required(VERSION 2.8); allow it under CMake 4.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(
        sentencepiece
        GIT_REPOSITORY https://github.com/google/sentencepiece.git
        GIT_TAG        v0.2.0
        GIT_SHALLOW    ON
        ${VASDK_FETCH_EXCLUDE}   # its own install rules stay out of VASDK's
    )
    FetchContent_MakeAvailable(sentencepiece)
    # It bundles an old protobuf-lite (its model files are protobuf messages). Compiled under its
    # own namespace, it can't collide with the protobuf VASDK links: on Android everything is one
    # .so, and two google::protobuf would clash at link time (or worse, at run time).
    # Every target of its own (the libraries and the spm_* tools), so they agree with each other.
    get_property(_spm_targets DIRECTORY ${sentencepiece_SOURCE_DIR}/src PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target ${_spm_targets})
        get_target_property(_type ${_target} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            target_compile_definitions(${_target} PRIVATE google=sentencepiece_google)
        endif()
    endforeach()
    if(ANDROID AND TARGET sentencepiece-static)
        target_link_libraries(sentencepiece-static INTERFACE log)
    endif()
endif()
FetchContent_GetProperties(sentencepiece)
