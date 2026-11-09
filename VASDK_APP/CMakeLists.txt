cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK_APP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# The garage agent's steps and envelopes written in C++, and garage_agentc: vasdk_agentc with them
# registered, which runs the agent's scripted conversations. Needs no audio, so before the checks below.
if(VASDK_ENABLE_ENGINE_CONFIG AND TARGET VASDK)
    set(GARAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/agents/garage-menu)
    add_library(garage_extensions STATIC ${GARAGE_DIR}/extensions/GarageExtensions.cpp ${GARAGE_DIR}/garage.proto)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/garage_proto)   # protoc doesn't create its output folder
    protobuf_generate(TARGET garage_extensions
        IMPORT_DIRS ${GARAGE_DIR}
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/garage_proto)
    target_include_directories(garage_extensions PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/garage_proto>
        $<BUILD_INTERFACE:${GARAGE_DIR}/extensions>)
    target_link_libraries(garage_extensions PUBLIC VASDK)

    add_executable(garage_agentc ${GARAGE_DIR}/extensions/garage_agentc.cpp)
    target_link_libraries(garage_agentc PRIVATE garage_extensions)
    if(TARGET vasdk_provider_catalog)   # --engine-config's sentence_encoder and entity_model (minilm, gliner)
        target_link_libraries(garage_agentc PRIVATE vasdk_provider_catalog)
        target_compile_definitions(garage_agentc PRIVATE VASDK_HAS_PROVIDER_CATALOG)
    endif()
    if(WIN32 AND TARGET VASDK)   # a VASDK.dll beside it is loaded first: keep it the current one
        get_target_property(_vasdk_type VASDK TYPE)
        if(_vasdk_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET garage_agentc POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:VASDK> $<TARGET_FILE_DIR:garage_agentc>)
        endif()
    endif()

    if(VASDK_BUILD_TESTS)
        file(GLOB GARAGE_MENU_TESTS ${GARAGE_DIR}/tests/*.txtpb)
        add_test(NAME vasdk_agent_garage_menu COMMAND garage_agentc test ${GARAGE_DIR}/agent.txtpb ${GARAGE_MENU_TESTS})
        # Intent accuracy and entity F1 on what callers say, misspellings included
        # The same suite from Python, its C++ step, envelope and tool written in Python (the C API)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(VASDK_BUILD_PYTHON_BINDING AND Python3_Interpreter_FOUND AND TARGET vasdk_c)
            add_test(NAME vasdk_python_garage
                     COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/bindings/python/tests/test_garage.py ${GARAGE_DIR})
            set_tests_properties(vasdk_python_garage PROPERTIES ENVIRONMENT "VASDK_LIBRARY=$<TARGET_FILE:vasdk_c>")
            # A page type and an entity extractor written in Python
            add_test(NAME vasdk_python_extensions
                     COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/bindings/python/tests/test_extensions.py)
            set_tests_properties(vasdk_python_extensions PROPERTIES ENVIRONMENT "VASDK_LIBRARY=$<TARGET_FILE:vasdk_c>")
            # Providers, processors, a runtime, and the conversation's controls, written in Python
            add_test(NAME vasdk_python_providers
                     COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/bindings/python/tests/test_providers.py)
            set_tests_properties(vasdk_python_providers PROPERTIES ENVIRONMENT "VASDK_LIBRARY=$<TARGET_FILE:vasdk_c>" TIMEOUT 120)
        endif()
        add_test(NAME vasdk_agent_garage_menu_nlu
                 COMMAND garage_agentc eval ${GARAGE_DIR}/agent.txtpb ${GARAGE_DIR}/nlu_eval.txtpb --min-accuracy 0.9)
        # The same set as Whistle heard it, said by three of Phonon's voices (vasdk_spoken_eval).
        add_test(NAME vasdk_agent_garage_menu_nlu_spoken
                 COMMAND garage_agentc eval ${GARAGE_DIR}/agent.txtpb ${GARAGE_DIR}/nlu_eval.spoken.txtpb --min-accuracy 0.9)
    endif()
endif()

# Check if required modules are enabled for VASDK_Pipeline
set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED TRUE)
set(VASDK_PIPELINE_MISSING_MODULES "")

if(NOT VASDK_BUILD_SDL AND NOT VASDK_BUILD_MINIAUDIO)
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "SDL or MINIAUDIO")
endif()

if(NOT VASDK_ENABLE_ENGINE_CONFIG)
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "ENGINE_CONFIG (VASDK_ENABLE_ENGINE_CONFIG)")
endif()

if(NOT VASDK_BUILD_SILERO_VAD)
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "SILERO_VAD")
endif()

# Require at least one transcription provider
if(NOT (VASDK_BUILD_GLADIAPP OR VASDK_BUILD_SONIOXPP_STT OR VASDK_BUILD_DEEPGRAMPP_STT OR VASDK_BUILD_CARTESIAPP_STT OR VASDK_BUILD_FISHAUDIOPP_STT OR VASDK_BUILD_GRADIUMPP_STT OR VASDK_BUILD_PARAKEET OR VASDK_BUILD_WHISPERCPP OR VASDK_BUILD_MOONSHINE_STT OR VASDK_BUILD_WHISTLE_STT))
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "At least one transcription provider (GLADIAPP, SONIOXPP_STT, DEEPGRAMPP_STT, CARTESIAPP_STT, FISHAUDIOPP_STT, GRADIUMPP_STT, PARAKEET, WHISPERCPP, MOONSHINE_STT, or WHISTLE_STT)")
endif()

# Require at least one TTS provider
if(NOT (VASDK_BUILD_DEEPGRAMPP_TTS OR VASDK_BUILD_CARTESIAPP_TTS OR VASDK_BUILD_FISHAUDIOPP_TTS OR VASDK_BUILD_GRADIUMPP_TTS OR VASDK_BUILD_SONIOXPP_TTS OR VASDK_BUILD_SUPERTONIC_TTS OR VASDK_BUILD_POCKET_TTS OR VASDK_BUILD_PHONON_TTS OR VASDK_BUILD_MOSS_TTS))
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "At least one TTS provider (DEEPGRAMPP_TTS, CARTESIAPP_TTS, FISHAUDIOPP_TTS, GRADIUMPP_TTS, SONIOXPP_TTS, SUPERTONIC_TTS, POCKET_TTS, PHONON_TTS, or MOSS_TTS)")
endif()

if(NOT VASDK_PIPELINE_REQUIRED_MODULES_ENABLED)
    message(WARNING "VASDK_Pipeline requires the following modules but they are disabled: ${VASDK_PIPELINE_MISSING_MODULES}")
    message(WARNING "Skipping VASDK_Pipeline build. Enable required modules with -DVASDK_BUILD_<MODULE>=ON")
    return()
endif()

# Collect TUI sources when FTXUI is available
set(VASDK_PIPELINE_TUI_SOURCES "")
if(VASDK_BUILD_TUI AND TARGET ftxui::component)
    list(APPEND VASDK_PIPELINE_TUI_SOURCES TUI/PipelineTUI.cpp)
endif()

# Add pipeline-mode executable with LLM support
add_executable(VASDK_Pipeline main_pipeline.cpp ${VASDK_PIPELINE_TUI_SOURCES})

# Link libraries conditionally based on enabled modules
target_link_libraries(VASDK_Pipeline
    PRIVATE VASDK
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE CLI11::CLI11
    PRIVATE dotenv
)

# Conditionally link 3rd party modules based on build options
if(VASDK_BUILD_MINIAUDIO AND TARGET vasdk_miniaudio_audio)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_miniaudio_audio)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_MINIAUDIO)
elseif(VASDK_BUILD_SDL AND TARGET vasdk_sdl_audio)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_sdl_audio)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SDL)
endif()

if(VASDK_BUILD_LIBDATACHANNEL_WEBRTC AND TARGET vasdk_libdatachannel_webrtc)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_libdatachannel_webrtc)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_LIBDATACHANNEL_WEBRTC
        VASDK_WEBRTC_DEMO_PAGE="${VASDK_ROOT_DIR}/assets/webrtc/index.html")
endif()

# VAD, STT and TTS providers: every one built in this tree, picked by id from --config
target_link_libraries(VASDK_Pipeline PRIVATE vasdk_provider_catalog)

# The example agent's steps and envelopes written in C++ (registered in main_pipeline.cpp)
target_link_libraries(VASDK_Pipeline PRIVATE garage_extensions)

if(VASDK_BUILD_FFMPEG_AUDIO AND TARGET vasdk_ffmpeg_audio)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_ffmpeg_audio)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_FFMPEG_AUDIO)
endif()

# Link the VizAnim provider if it was built
if(VASDK_BUILD_VIZANIM AND TARGET vasdk_vizanim)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_vizanim)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_VIZANIM)
    message(STATUS "[VASDK_Pipeline] VizAnim audio feature extraction enabled")
endif()

# Link the pyannote speaker embedding provider if it was built
if(VASDK_BUILD_PYANNOTE_SPEAKER AND TARGET vasdk_pyannote_speaker)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_pyannote_speaker)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_PYANNOTE_SPEAKER)
    message(STATUS "[VASDK_Pipeline] pyannote speaker tracking enabled")
endif()

if(VASDK_BUILD_TUI AND TARGET ftxui::component)
    target_link_libraries(VASDK_Pipeline PRIVATE ftxui::component ftxui::dom ftxui::screen)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_FTXUI)
    target_include_directories(VASDK_Pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    message(STATUS "[VASDK_Pipeline] FTXUI terminal UI enabled")
endif()

if(VASDK_BUILD_LLAMACPP AND TARGET vasdk_llamacpp)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_llamacpp)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_LLAMACPP)
    message(STATUS "[VASDK_Pipeline] llama.cpp local LLM provider enabled")
endif()

# Copy runtime DLLs to output directory for VASDK_Pipeline
if(WIN32)
    # Copy ONNX Runtime DLLs
    add_custom_command(TARGET VASDK_Pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
        $<TARGET_FILE_DIR:VASDK_Pipeline>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${onnxruntime_SOURCE_DIR}/lib/onnxruntime_providers_shared.dll"
        $<TARGET_FILE_DIR:VASDK_Pipeline>
    )
    # Copy SDL3 DLL (only when SDL is enabled)
    if(VASDK_BUILD_SDL AND TARGET SDL3::SDL3)
        add_custom_command(TARGET VASDK_Pipeline POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:VASDK_Pipeline>
        )
    endif()
    # Copy llama.cpp runtime DLLs (llama, ggml, ggml-base, ggml-cpu, pffft).
    # After the BUILD_SHARED_LIBS=OFF fix these will be static and this loop
    # becomes a no-op; it is kept as a safety net for pre-existing shared builds.
    if(VASDK_BUILD_LLAMACPP)
        foreach(_llama_tgt llama ggml ggml-base ggml-cpu PFFFT)
            if(TARGET ${_llama_tgt})
                get_target_property(_llama_tgt_type ${_llama_tgt} TYPE)
                if(_llama_tgt_type STREQUAL "SHARED_LIBRARY")
                    add_custom_command(TARGET VASDK_Pipeline POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "$<TARGET_FILE:${_llama_tgt}>"
                            "$<TARGET_FILE_DIR:VASDK_Pipeline>"
                    )
                endif()
            endif()
        endforeach()
    endif()
    # Copy VASDK DLL and its runtime dependencies
    add_custom_command(TARGET VASDK_Pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:VASDK>
        $<TARGET_FILE_DIR:VASDK_Pipeline>
    )
    # An installed libcurl (a package manager's) is a DLL that VASDK.dll imports, along with
    # zlib; a libcurl built from source (VASDK_CURL_BUNDLED) is linked statically.
    if(VASDK_ENABLE_HTTP_TRANSPORT AND NOT VASDK_CURL_BUNDLED)
        add_custom_command(TARGET VASDK_Pipeline POST_BUILD
            # debug: libcurl-d.dll, release: libcurl.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,libcurl-d.dll,libcurl.dll>"
                $<TARGET_FILE_DIR:VASDK_Pipeline>
            # debug: zlibd1.dll, release: zlib1.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,zlibd1.dll,zlib1.dll>"
                $<TARGET_FILE_DIR:VASDK_Pipeline>
        )
    endif()
endif()

if(VASDK_ENABLE_CTRACK AND TARGET vasdk_ctrack_iface)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_ctrack_iface)
endif()

install(TARGETS VASDK_Pipeline COMPONENT apps EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install model files to the installation directory
install(DIRECTORY "${VASDK_ROOT_BINARY_DIR}/models/"
    DESTINATION "bin/models/"
    OPTIONAL
    COMPONENT apps EXCLUDE_FROM_ALL
)

# Post-build step to copy all shared libraries to the install directory
install(CODE "
    # Find and install all shared libraries in build directories
    file(GLOB_RECURSE SHARED_LIBS
        \"${VASDK_ROOT_BINARY_DIR}/*.dll\"
        \"${VASDK_ROOT_BINARY_DIR}/*.so\"
        \"${VASDK_ROOT_BINARY_DIR}/*.dylib\"
    )
    foreach(lib \${SHARED_LIBS})
        get_filename_component(lib_name \${lib} NAME)
        # Skip system libraries and debug symbols
        if(NOT lib_name MATCHES \"(msvc|ucrt|api-ms|ext-ms|.*d\\.dll)\" AND
           NOT lib MATCHES \"CMakeFiles\")
            file(INSTALL \${lib} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")
        endif()
    endforeach()
"
    COMPONENT apps EXCLUDE_FROM_ALL
)
