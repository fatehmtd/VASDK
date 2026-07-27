# 
# FetchRNNoise.cmake
#
# Downloads the xiph/rnnoise C source and pre-trained model weights, then
# creates the CMake target `rnnoise_lib`.
# Included by VASDK_3P_IMPL/CMakeLists.txt when VASDK_BUILD_RNNOISE is ON.
#
# RNNoise ships with autotools only (no CMakeLists.txt), so we use
# FetchContent_Populate to download the sources and wire them up manually.
#
# The model weights (rnnoise_data.c / rnnoise_data.h) are NOT in the git repo -
# they must be downloaded separately from media.xiph.org.  The hash of the
# expected archive is recorded in the repo's `model_version` file.
# 

# TODO: rnnoise has no tagged releases - pin to a specific commit SHA before release
# e.g.: GIT_TAG <commit-sha>
FetchContent_Declare(
    rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG        main
    GIT_SHALLOW    ON
)

# Populate (download) without trying to add_subdirectory, because rnnoise
# has no CMakeLists.txt. The same pattern is used for 'supertonic'.
FetchContent_Populate(rnnoise)

if(NOT TARGET rnnoise_lib)

    #  Step 1: Download pre-trained model weights 
    # The repo records the expected SHA-256 of the model archive in model_version.
    file(READ "${rnnoise_SOURCE_DIR}/model_version" RNNOISE_MODEL_HASH)
    string(STRIP "${RNNOISE_MODEL_HASH}" RNNOISE_MODEL_HASH)

    set(RNNOISE_MODEL_ARCHIVE
        "${CMAKE_CURRENT_BINARY_DIR}/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz")
    set(RNNOISE_MODEL_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/rnnoise_model")
    set(RNNOISE_DATA_C "${RNNOISE_MODEL_DIR}/src/rnnoise_data.c")
    set(RNNOISE_DATA_H_DIR "${RNNOISE_MODEL_DIR}/src")

    if(NOT EXISTS "${RNNOISE_DATA_C}")
        set(RNNOISE_MODEL_URL
            "https://media.xiph.org/rnnoise/models/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz")

        message(STATUS "[RNNoise] Downloading model weights...")
        message(STATUS "[RNNoise]   URL: ${RNNOISE_MODEL_URL}")

        file(DOWNLOAD
            "${RNNOISE_MODEL_URL}"
            "${RNNOISE_MODEL_ARCHIVE}"
            SHOW_PROGRESS
            STATUS RNNOISE_DL_STATUS
            TIMEOUT 120
        )
        list(GET RNNOISE_DL_STATUS 0 RNNOISE_DL_CODE)
        if(NOT RNNOISE_DL_CODE EQUAL 0)
            list(GET RNNOISE_DL_STATUS 1 RNNOISE_DL_MSG)
            message(FATAL_ERROR "[RNNoise] Model download failed: ${RNNOISE_DL_MSG}")
        endif()

        file(MAKE_DIRECTORY "${RNNOISE_MODEL_DIR}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf "${RNNOISE_MODEL_ARCHIVE}"
            WORKING_DIRECTORY "${RNNOISE_MODEL_DIR}"
            RESULT_VARIABLE RNNOISE_EXTRACT_RESULT
        )
        if(NOT RNNOISE_EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "[RNNoise] Failed to extract model archive")
        endif()

        message(STATUS "[RNNoise] Model weights extracted to ${RNNOISE_MODEL_DIR}")
    else()
        message(STATUS "[RNNoise] Model weights already present, skipping download")
    endif()

    #  Step 2: Collect library sources (exclude standalone tool executables) 
    # Only files with an active (non-dead-code) main() are excluded.
    # Note: parse_lpcnet_weights.c has main() inside #if 0 - it IS a library file.
    set(RNNOISE_TOOL_SOURCES
        "${rnnoise_SOURCE_DIR}/src/write_weights.c"       # main(): weights generator
        "${rnnoise_SOURCE_DIR}/src/dump_features.c"       # main(): feature dump tool
        "${rnnoise_SOURCE_DIR}/src/dump_rnnoise_tables.c" # main(): table dump tool
    )

    file(GLOB RNNOISE_SOURCES "${rnnoise_SOURCE_DIR}/src/*.c")
    list(REMOVE_ITEM RNNOISE_SOURCES ${RNNOISE_TOOL_SOURCES})

    # Add the downloaded model data (not in the git repo)
    list(APPEND RNNOISE_SOURCES "${RNNOISE_DATA_C}")

    if(NOT RNNOISE_SOURCES)
        message(FATAL_ERROR
            "[RNNoise] No library C sources found in ${rnnoise_SOURCE_DIR}/src/.")
    endif()

    #  Step 3: Build the static library 
    add_library(rnnoise_lib STATIC ${RNNOISE_SOURCES})

    target_include_directories(rnnoise_lib
        PUBLIC  "${rnnoise_SOURCE_DIR}/include"
        PRIVATE "${rnnoise_SOURCE_DIR}/src"
        PRIVATE "${RNNOISE_DATA_H_DIR}"   # rnnoise_data.h lives here
    )

    if(MSVC)
        target_compile_options(rnnoise_lib PRIVATE
            /W0                      # silence all warnings in third-party code
        )
        target_compile_definitions(rnnoise_lib PRIVATE
            _USE_MATH_DEFINES        # M_PI etc. on MSVC
            _CRT_SECURE_NO_WARNINGS
        )
    else()
        target_compile_options(rnnoise_lib PRIVATE -w)
    endif()

    set_target_properties(rnnoise_lib PROPERTIES
        C_STANDARD            99
        POSITION_INDEPENDENT_CODE ON
        # Use dynamic CRT (/MD, /MDd) to match the rest of the VASDK build
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )

    message(STATUS "[RNNoise] rnnoise_lib target created (${rnnoise_SOURCE_DIR})")
endif()
