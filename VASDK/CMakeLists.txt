cmake_minimum_required(VERSION 3.21...4.0)

# The SDK's one version, from the VERSION file (the top-level project reads it, Android's too)
if(NOT DEFINED VASDK_VERSION)
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION VASDK_VERSION LIMIT_COUNT 1)
endif()
project(VASDK VERSION ${VASDK_VERSION} LANGUAGES CXX)

# The repository's root and its build, when this directory is added on its own
if(NOT DEFINED VASDK_ROOT_DIR)
    get_filename_component(VASDK_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
endif()
if(NOT DEFINED VASDK_ROOT_BINARY_DIR)
    get_filename_component(VASDK_ROOT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/.. ABSOLUTE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#  Metrics feature options 
option(VASDK_METRICS_PROMETHEUS "Build Prometheus /metrics HTTP exporter"      OFF)
option(VASDK_METRICS_STATSD     "Build StatsD UDP metrics transport"           OFF)
option(VASDK_METRICS_INFLUX     "Build InfluxDB v2 Line Protocol transport"    OFF)
option(VASDK_METRICS_WEBSOCKET  "Build WebSocket metrics push transport"       OFF)


# Engine config files (protobuf schema, loaders, provider registry), agents and VAEngine.
# Full protobuf on every platform, Android included (gate G1: about +2.6 MB of native code).
option(VASDK_ENABLE_ENGINE_CONFIG "Build engine config files (protobuf) and the provider registry" ON)
if(VASDK_ENABLE_ENGINE_CONFIG)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/VASDKProtobuf.cmake)
endif()
# The C++ library is static by default: apps link it whole, and the C library (vasdk_c) is built
# from it, a shared library of its own. VASDK_SHARED makes it a shared library instead, which needs
# a shared protobuf: one protobuf per binary, so a static protobuf (built from source, as on
# Android) keeps VASDK static, and it and the app's own generated code (provider options,
# extensions) share the one copy linked into the app. Two copies would keep two registries of
# message types.
option(VASDK_SHARED "Build the C++ library, VASDK, as a shared library (needs a shared protobuf)" OFF)
set(VASDK_LIBRARY_TYPE STATIC)
if(VASDK_SHARED)
    set(VASDK_LIBRARY_TYPE SHARED)
    if(TARGET protobuf::libprotobuf)
        get_target_property(_protobuf_type protobuf::libprotobuf TYPE)
        if(_protobuf_type STREQUAL "STATIC_LIBRARY")
            message(WARNING "VASDK_SHARED: protobuf is a static library here, so VASDK stays static (one protobuf per binary). "
                            "Install a shared protobuf to build VASDK shared.")
            set(VASDK_LIBRARY_TYPE STATIC)
        endif()
    endif()
endif()

# The model manifest (VASDK_3P_IMPL/models/manifest.json), built in: what ModelStore downloads
# when it is given no other. Its bytes, like the language pack below.
set(_vasdk_model_manifest ${VASDK_ROOT_DIR}/VASDK_3P_IMPL/models/manifest.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_vasdk_model_manifest})
file(READ ${_vasdk_model_manifest} _vasdk_model_manifest_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _vasdk_model_manifest_bytes "${_vasdk_model_manifest_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/ModelManifest.cpp.in
    "// Generated from VASDK_3P_IMPL/models/manifest.json by CMake: edit that file (generate_manifest.py).\n"
    "#include <VASDK/ModelStore.h>\n"
    "namespace va {\n"
    "std::string builtinModelManifest() {\n"
    "    const unsigned char bytes[] = {${_vasdk_model_manifest_bytes}};\n"
    "    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));\n"
    "}\n"
    "}\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/ModelManifest.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/ModelManifest.cpp COPYONLY)

add_library(VASDK ${VASDK_LIBRARY_TYPE}
    # Core sources
    ./src/AudioRingBuffer.cpp
    ./src/AudioSIMD.cpp
    ./src/ModelStore.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ModelManifest.cpp
    # Transport infrastructure
    ./src/Transport.cpp
    ./src/HttpTransport.cpp
    ./src/LocalTransport.cpp
    ./src/WebSocketTransport.cpp
    ./src/WebRtcTransport.cpp
    ./src/MockTransport.cpp
    ./src/TransportFactory.cpp
    # LLM infrastructure (va::llm namespace)
    ./src/llm/LLMTransport.cpp
    ./src/llm/OpenAITransport.cpp
    ./src/llm/Completion.cpp
    ./src/llm/AnthropicTransport.cpp
    ./src/llm/StreamingReplyExtractor.cpp
    ./src/llm/PromptTemplate.cpp
    ./src/llm/ApiPromptTemplates.cpp
    ./src/llm/LocalPromptTemplates.cpp
    ./src/llm/Tool.cpp
    # Frame processors
    ./src/FrameProcessor.cpp
    ./src/AudioBufferProcessor.cpp
    ./src/TransportInputProcessor.cpp
    ./src/TransportOutputProcessor.cpp
    ./src/VADFrameProcessor.cpp
    ./src/TranscriptionProcessor.cpp
    ./src/TTSFrameProcessor.cpp
    ./src/QueuedVoiceSynthesisProvider.cpp
    ./src/llm/LLMProcessor.cpp
    ./src/llm/AggregatingLLMProcessor.cpp
    ./src/llm/ToolDispatchProcessor.cpp
    ./src/AudioDSPProcessor.cpp
    ./src/Pipeline.cpp
    ./src/TurnDetectionProcessor.cpp
    ./src/TurnAssemblerProcessor.cpp
    ./src/Masking.cpp
    ./src/WakeWordProcessor.cpp
    ./src/SpeakerIdentificationProcessor.cpp
    ./src/AgentStateProcessor.cpp
    # Goal-oriented call center framework (va::goal namespace)
    ./src/goal/GoalState.cpp
    ./src/goal/GoalManager.cpp
    ./src/goal/IntentRouter.cpp
    ./src/goal/GoalStatePersistence.cpp
    ./src/goal/ConditionEvaluator.cpp
    ./src/goal/GoalDefinitionLoader.cpp
    ./src/goal/GoalConversationSimulator.cpp
    ./src/RecoveryProcessor.cpp
    # Context providers
    ./src/context/ContextProvider.cpp
    ./src/context/AppContextProvider.cpp
    ./src/context/LocationContextProvider.cpp
    ./src/context/NetworkContextProvider.cpp
    ./src/context/ToolsContextProvider.cpp
    ./src/context/SystemContextProvider.cpp
    ./src/context/ContextAggregator.cpp
    # Metrics infrastructure (always built)
    ./src/MetricsCollector.cpp
    ./src/MetricsReporter.cpp
)

add_library(VASDK::VASDK ALIAS VASDK)   # the C++ API, for a project that adds VASDK from source

target_link_libraries(VASDK
    PUBLIC spdlog::spdlog
)

# The C API's functions (VASDK/c/vasdk_c_export.h): exported from VASDK, and from the C library
# built from it. Code linked to a static VASDK calls them directly.
target_compile_definitions(VASDK PRIVATE VASDK_C_BUILDING)
if(VASDK_LIBRARY_TYPE STREQUAL "STATIC")
    target_compile_definitions(VASDK PUBLIC VASDK_STATIC_DEFINE INTERFACE VASDK_C_STATIC)
    # Default visibility, as the code linking it: the C library limits what it exports with its own
    # lists (cmake/c_library). Hidden here and default there, macOS sees two copies of a type's
    # identity, so a provider registered by an app (ProviderRegistry, keyed by typeid) isn't found.
endif()

#  Optional metrics exporters 
if(VASDK_METRICS_STATSD)
    target_sources(VASDK PRIVATE ./src/metrics/StatsDMetricsTransport.cpp)
    target_compile_definitions(VASDK PUBLIC VASDK_METRICS_STATSD)
    message(STATUS "VASDK metrics: StatsD UDP exporter enabled")
endif()

if(VASDK_METRICS_INFLUX)
    target_sources(VASDK PRIVATE ./src/metrics/InfluxMetricsTransport.cpp)
    target_compile_definitions(VASDK PUBLIC VASDK_METRICS_INFLUX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/VASDKCurl.cmake)
    target_link_libraries(VASDK PRIVATE CURL::libcurl)
    message(STATUS "VASDK metrics: InfluxDB exporter enabled")
endif()

if(VASDK_METRICS_WEBSOCKET)
    target_sources(VASDK PRIVATE ./src/metrics/WebSocketMetricsTransport.cpp)
    target_compile_definitions(VASDK PUBLIC VASDK_METRICS_WEBSOCKET)
    message(STATUS "VASDK metrics: WebSocket push exporter enabled")
endif()

if(VASDK_METRICS_PROMETHEUS)
    # cpp-httplib fetch and link are handled by the shared block below
    target_sources(VASDK PRIVATE ./src/metrics/PrometheusExporter.cpp)
    target_compile_definitions(VASDK PUBLIC VASDK_METRICS_PROMETHEUS)
    message(STATUS "VASDK metrics: Prometheus HTTP exporter enabled")
endif()

#  Google Highway SIMD acceleration 
# A binary built with AVX2 needs a CPU with AVX2 (Intel Haswell, AMD Excavator, 2013 or later):
# packages for every machine turn it off (the release presets).
option(VASDK_ENABLE_AVX2 "Compile VASDK's audio helpers for AVX2 and FMA on x86_64 (GCC and Clang)" ON)
if(VASDK_ENABLE_HIGHWAY AND (TARGET hwy::hwy OR TARGET hwy))
    if(TARGET hwy::hwy)
        target_link_libraries(VASDK PRIVATE hwy::hwy)
    else()
        target_link_libraries(VASDK PRIVATE hwy)
    endif()
    target_compile_definitions(VASDK PRIVATE VASDK_HIGHWAY_ENABLED)
    # On x86_64 desktop, enable AVX2+FMA so Highway picks a wider ISA than SSE2 (MSVC has no such flags)
    if(VASDK_ENABLE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT ANDROID AND NOT MSVC)
        target_compile_options(VASDK PRIVATE -mavx2 -mfma)
    endif()
    message(STATUS "VASDK: Google Highway SIMD acceleration enabled")
else()
    message(STATUS "VASDK: Google Highway not available - using scalar audio helpers")
endif()

# Optional: Link nlohmann_json if available (for WebSocketTransport and LLMTransport serialization)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(VASDK PUBLIC nlohmann_json::nlohmann_json)   # public headers (llm/Tool.h, flow/Tools.h) use it
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_JSON)
    message(STATUS "nlohmann_json found - WebSocketTransport and LLMTransport will use JSON serialization")
else()
    message(STATUS "nlohmann_json not found - WebSocketTransport and LLMTransport will use stub serialization")
endif()

#  cpp-httplib (shared between Prometheus exporter and WebSocket server) 
option(VASDK_ENABLE_WEBSOCKET_SERVER "Enable WebSocket server transport via cpp-httplib" ON)

if(VASDK_METRICS_PROMETHEUS OR VASDK_ENABLE_WEBSOCKET_SERVER)
    # An installed one under VASDK_DEPENDENCY_SOURCE SYSTEM or AUTO (Conan, distro packages), else the pinned release
    vasdk_dependency(httplib TARGET httplib::httplib
        FIND CONFIG
        DECLARE GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
                GIT_TAG        v0.18.3)
    target_link_libraries(VASDK PRIVATE httplib::httplib)
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_HTTPLIB)
    message(STATUS "VASDK: cpp-httplib available (Prometheus=${VASDK_METRICS_PROMETHEUS}, WebSocket=${VASDK_ENABLE_WEBSOCKET_SERVER})")
endif()

if(VASDK_ENABLE_WEBSOCKET_SERVER)
    target_sources(VASDK PRIVATE ./src/network/CppHttpLibWebSocketProvider.cpp)
    message(STATUS "VASDK: WebSocket server provider enabled")
endif()

# Find and link libcurl for HTTP transports (optional - disable with -DVASDK_ENABLE_HTTP_TRANSPORT=OFF)
option(VASDK_ENABLE_HTTP_TRANSPORT "Enable HTTP-based transports (libcurl, built from source if not installed)" ON)
if(VASDK_ENABLE_HTTP_TRANSPORT)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/VASDKCurl.cmake)
    target_link_libraries(VASDK PRIVATE CURL::libcurl)
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_CURL)
    target_sources(VASDK PRIVATE ./src/network/CurlHttpProvider.cpp)
    message(STATUS "libcurl available - HTTP transport support enabled")
else()
    message(STATUS "VASDK_ENABLE_HTTP_TRANSPORT=OFF - skipping libcurl, HTTP transports will stub out")
endif()

#  Engine config sources (see VASDK_ENABLE_ENGINE_CONFIG above)
if(VASDK_ENABLE_ENGINE_CONFIG)
    # Generated code is compiled into VASDK only: linking it twice would register the
    # descriptors twice and abort at startup.
    target_sources(VASDK PRIVATE
        ./src/engine/ProtoFileLoader.cpp
        ./src/engine/EventTapProcessor.cpp
        ./src/engine/Runtime.cpp
        ./src/engine/Session.cpp
        ./src/SessionManager.cpp
        ./src/VAEngine.cpp
        ./src/flow/Expression.cpp
        ./src/flow/SlotExtraction.cpp
        ./src/flow/Entities.cpp
        ./src/flow/CompiledAgent.cpp
        ./src/flow/FlowRegistry.cpp
        ./src/flow/AgentTool.cpp
        ./src/flow/FlowRuntime.cpp
        ./src/flow/DialogProcessor.cpp
        ./src/flow/FlowTestRunner.cpp
        ./src/flow/Tools.cpp
        ./src/nlu/NluProvider.cpp
        ./src/nlu/RuleNluProvider.cpp
        ./src/nlu/Calendar.cpp
        ./src/nlu/Language.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltInLanguage.cpp
        ./src/nlu/EmbeddingNluProvider.cpp
        ./src/nlu/LlmNluProvider.cpp
        ./src/nlu/EntityExtractor.cpp
        ./src/nlu/LlmEntityExtractor.cpp
        ./src/nlu/TaggerEntityExtractor.cpp
        ./src/c/vasdk_c.cpp
        ./src/c/vasdk_c_session.cpp
        ./src/c/vasdk_c_providers.cpp
        ./src/c/vasdk_c_models.cpp
        ./src/engine/RemoteControl.cpp
        ./proto/vasdk/engine/engine_config.proto
        ./proto/vasdk/engine/events.proto
        ./proto/vasdk/nlu/entity.proto
        ./proto/vasdk/nlu/language.proto
        ./proto/vasdk/flow/expectation.proto
        ./proto/vasdk/flow/trigger.proto
        ./proto/vasdk/goal/goal.proto
        ./proto/vasdk/flow/agent.proto
        ./proto/vasdk/flow/page_types.proto
        ./proto/vasdk/flow/test_case.proto
        ./proto/vasdk/flow/step_outcome.proto
        ./proto/vasdk/flow/conversation.proto
        ./proto/vasdk/engine/session.proto
        ./proto/vasdk/flow/snapshot.proto
    )
    # VASDK's own language pack (languages/en-US.txtpb), built in: what an agent that names no
    # language speaks (nlu::Language::builtIn). Its bytes, so any text goes in unescaped.
    set(_vasdk_language ${CMAKE_CURRENT_SOURCE_DIR}/languages/en-US.txtpb)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_vasdk_language})
    file(READ ${_vasdk_language} _vasdk_language_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _vasdk_language_bytes "${_vasdk_language_hex}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltInLanguage.cpp.in
        "// Generated from languages/en-US.txtpb by CMake: edit that file.\n"
        "#include <string>\n"
        "namespace va { namespace nlu {\n"
        "std::string builtInLanguagePack() {\n"
        "    const unsigned char bytes[] = {${_vasdk_language_bytes}};\n"
        "    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));\n"
        "}\n"
        "} }\n")
    configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltInLanguage.cpp.in
                   ${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltInLanguage.cpp COPYONLY)
    # A Windows DLL exports only what's marked: the generated messages are marked VASDK_EXPORT, so
    # apps and tests can use vasdk::flow::Agent, vasdk::engine::Event... The generated headers
    # don't include vasdk_export.h, so every file that may include them gets it first.
    set(_vasdk_proto_export)
    if(WIN32 AND VASDK_LIBRARY_TYPE STREQUAL "SHARED")
        set(_vasdk_proto_export EXPORT_MACRO VASDK_EXPORT)
        target_compile_options(VASDK PUBLIC
            "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/FIVASDK/vasdk_export.h>"
            "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:SHELL:-include VASDK/vasdk_export.h>")
    endif()
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto)   # protoc doesn't create its output folder
    protobuf_generate(TARGET VASDK
        ${_vasdk_proto_export}
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto ${VASDK_PROTOBUF_IMPORT_DIRS}
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
    target_include_directories(VASDK PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/proto> $<INSTALL_INTERFACE:include/VASDK/proto>)
    # The same messages for Python (bindings/python: vasdk.engine.events_pb2, ...), by the same
    # protoc, into build/bindings/python/vasdk, where the binding looks for them (bundle.py copies
    # them into a wheel). Python's protobuf package must be this protoc's version or newer.
    if(NOT ANDROID AND (TARGET protobuf::protoc OR Protobuf_PROTOC_EXECUTABLE))
        if(TARGET protobuf::protoc)
            set(_vasdk_protoc $<TARGET_FILE:protobuf::protoc>)
        else()
            set(_vasdk_protoc ${Protobuf_PROTOC_EXECUTABLE})
        endif()
        set(_vasdk_python_out ${VASDK_ROOT_BINARY_DIR}/bindings/python/vasdk)
        get_target_property(_vasdk_protos VASDK SOURCES)
        list(FILTER _vasdk_protos INCLUDE REGEX "[.]proto$")
        set(_vasdk_python_protos)
        set(_vasdk_python_inputs)
        set(_vasdk_python_depends)
        foreach(_proto IN LISTS _vasdk_protos)
            string(REGEX REPLACE "^[.]/proto/" "" _relative ${_proto})
            string(REGEX REPLACE "[.]proto$" "_pb2.py" _module ${_relative})
            string(REGEX REPLACE "^vasdk/" "" _module ${_module})
            list(APPEND _vasdk_python_protos ${_vasdk_python_out}/${_module})
            list(APPEND _vasdk_python_inputs ${_relative})
            list(APPEND _vasdk_python_depends ${CMAKE_CURRENT_SOURCE_DIR}/proto/${_relative})
            get_filename_component(_package ${_vasdk_python_out}/${_module} DIRECTORY)
            if(NOT EXISTS ${_package}/__init__.py)
                file(WRITE ${_package}/__init__.py "")
            endif()
        endforeach()
        add_custom_command(OUTPUT ${_vasdk_python_protos}
            COMMAND ${_vasdk_protoc} --python_out=${VASDK_ROOT_BINARY_DIR}/bindings/python --pyi_out=${VASDK_ROOT_BINARY_DIR}/bindings/python
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/proto ${VASDK_PROTOC_IMPORT_ARGUMENTS} ${_vasdk_python_inputs}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/proto
            DEPENDS ${_vasdk_python_depends}
            COMMENT "Python messages for bindings/python"
            VERBATIM)
        add_custom_target(vasdk_python_protos ALL DEPENDS ${_vasdk_python_protos})
    endif()
    target_link_libraries(VASDK PUBLIC protobuf::libprotobuf)
    target_compile_definitions(VASDK PUBLIC VASDK_HAS_ENGINE_CONFIG)
    target_compile_definitions(VASDK PRIVATE VASDK_C_VERSION="${PROJECT_VERSION}")
    # Checks, tests and chats with agent files: vasdk_agentc lint|test|chat agent.txtpb
    if(NOT ANDROID)   # a desktop tool
    add_executable(vasdk_agentc ./tools/vasdk_agentc/main.cpp)
    target_link_libraries(vasdk_agentc PRIVATE VASDK)
    endif()
    message(STATUS "VASDK: engine config files enabled")
endif()

set_target_properties(VASDK PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# The C++ library's export macros, <VASDK/vasdk_export.h>, and the version, <VASDK/version.h>,
# generated beside each other: VASDK_EXPORT, and VASDK_VERSION_MAJOR, _MINOR, _PATCH, _STRING
include(GenerateExportHeader)
generate_export_header(VASDK
    BASE_NAME VASDK
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/VASDK/vasdk_export.h
)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/VASDK/version.h @ONLY)

target_include_directories(VASDK PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>   # vasdk_export.h, version.h
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/VASDK # to simplify the internal include files
)

if(VASDK_ENABLE_CTRACK AND TARGET vasdk_ctrack_iface)
    target_link_libraries(VASDK PRIVATE vasdk_ctrack_iface)
endif()

# The C++ library and its headers, for a build that installs the C++ API (component
# vasdk_cpp_development, never part of a plain install: the packages ship the C library,
# cmake/c_library). Its dependencies' headers (protobuf, spdlog, nlohmann_json) are the consumer's.
include(GNUInstallDirs)
install(TARGETS VASDK COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/VASDK
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VASDK/proto
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    FILES_MATCHING PATTERN "*.pb.h"
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/VASDK/vasdk_export.h ${CMAKE_CURRENT_BINARY_DIR}/include/VASDK/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VASDK
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)

# Unit tests (run with ctest)
if(VASDK_BUILD_TESTS)
    add_executable(vasdk_streaming_reply_extractor_test tests/streaming_reply_extractor_test.cpp)
    target_link_libraries(vasdk_streaming_reply_extractor_test PRIVATE VASDK)
    add_test(NAME vasdk_streaming_reply_extractor COMMAND vasdk_streaming_reply_extractor_test)

    add_executable(vasdk_tts_turn_test tests/tts_turn_test.cpp)
    target_link_libraries(vasdk_tts_turn_test PRIVATE VASDK)
    add_test(NAME vasdk_tts_turn COMMAND vasdk_tts_turn_test)

    add_executable(vasdk_llm_stream_speech_test tests/llm_stream_speech_test.cpp)
    target_link_libraries(vasdk_llm_stream_speech_test PRIVATE VASDK nlohmann_json::nlohmann_json)
    add_test(NAME vasdk_llm_stream_speech COMMAND vasdk_llm_stream_speech_test)

    add_executable(vasdk_webrtc_transport_test tests/webrtc_transport_test.cpp)
    target_link_libraries(vasdk_webrtc_transport_test PRIVATE VASDK)
    add_test(NAME vasdk_webrtc_transport COMMAND vasdk_webrtc_transport_test)

    add_executable(vasdk_vad_turn_end_test tests/vad_turn_end_test.cpp)
    target_link_libraries(vasdk_vad_turn_end_test PRIVATE VASDK)
    add_test(NAME vasdk_vad_turn_end COMMAND vasdk_vad_turn_end_test)

    add_executable(vasdk_backchannel_test tests/backchannel_test.cpp)
    target_link_libraries(vasdk_backchannel_test PRIVATE VASDK)
    add_test(NAME vasdk_backchannel COMMAND vasdk_backchannel_test)

    add_executable(vasdk_pipeline_description_test tests/pipeline_description_test.cpp)
    target_link_libraries(vasdk_pipeline_description_test PRIVATE VASDK)
    add_test(NAME vasdk_pipeline_description COMMAND vasdk_pipeline_description_test)

    add_executable(vasdk_goal_test tests/goal_test.cpp)
    target_link_libraries(vasdk_goal_test PRIVATE VASDK nlohmann_json::nlohmann_json)
    add_test(NAME vasdk_goal COMMAND vasdk_goal_test)

    add_executable(vasdk_turn_assembler_test tests/turn_assembler_test.cpp)
    target_link_libraries(vasdk_turn_assembler_test PRIVATE VASDK)
    add_test(NAME vasdk_turn_assembler COMMAND vasdk_turn_assembler_test)

    add_executable(vasdk_tool_dispatch_race_test tests/tool_dispatch_race_test.cpp)
    target_link_libraries(vasdk_tool_dispatch_race_test PRIVATE VASDK nlohmann_json::nlohmann_json)
    add_test(NAME vasdk_tool_dispatch_race COMMAND vasdk_tool_dispatch_race_test)

    if(VASDK_ENABLE_ENGINE_CONFIG)
        add_executable(vasdk_proto_load_test tests/proto_load_test.cpp)
        target_link_libraries(vasdk_proto_load_test PRIVATE VASDK)
        add_test(NAME vasdk_proto_load COMMAND vasdk_proto_load_test)

        add_executable(vasdk_provider_registry_test tests/provider_registry_test.cpp)
        target_link_libraries(vasdk_provider_registry_test PRIVATE VASDK)
        add_test(NAME vasdk_provider_registry COMMAND vasdk_provider_registry_test)

        add_executable(vasdk_engine_assembly_test tests/engine_assembly_test.cpp)
        target_link_libraries(vasdk_engine_assembly_test PRIVATE VASDK)
        add_test(NAME vasdk_engine_assembly COMMAND vasdk_engine_assembly_test)

        add_executable(vasdk_activation_test tests/activation_test.cpp)
        target_link_libraries(vasdk_activation_test PRIVATE VASDK)
        add_test(NAME vasdk_activation COMMAND vasdk_activation_test)

        add_executable(vasdk_follow_up_window_test tests/follow_up_window_test.cpp)
        target_link_libraries(vasdk_follow_up_window_test PRIVATE VASDK)
        add_test(NAME vasdk_follow_up_window COMMAND vasdk_follow_up_window_test)

        add_executable(vasdk_session_test tests/session_test.cpp)
        target_link_libraries(vasdk_session_test PRIVATE VASDK)
        add_test(NAME vasdk_session COMMAND vasdk_session_test)
        # The config's audio devices and LLM provider, the new Commands, a StepOutcome's question
        add_executable(vasdk_config_providers_test tests/config_providers_test.cpp)
        target_link_libraries(vasdk_config_providers_test PRIVATE VASDK)
        add_test(NAME vasdk_config_providers COMMAND vasdk_config_providers_test)

        add_executable(vasdk_expression_test tests/expression_test.cpp)
        target_link_libraries(vasdk_expression_test PRIVATE VASDK)
        add_test(NAME vasdk_expression COMMAND vasdk_expression_test)

        add_executable(vasdk_entity_model_test tests/entity_model_test.cpp)
        target_link_libraries(vasdk_entity_model_test PRIVATE VASDK)
        add_test(NAME vasdk_entity_model COMMAND vasdk_entity_model_test)

        add_executable(vasdk_dialog_test tests/dialog_test.cpp)
        target_link_libraries(vasdk_dialog_test PRIVATE VASDK nlohmann_json::nlohmann_json)
        add_test(NAME vasdk_dialog COMMAND vasdk_dialog_test)

        add_executable(vasdk_form_primitives_test tests/form_primitives_test.cpp)
        target_link_libraries(vasdk_form_primitives_test PRIVATE VASDK)
        add_test(NAME vasdk_form_primitives COMMAND vasdk_form_primitives_test)

        add_executable(vasdk_entity_test tests/entity_test.cpp)
        target_link_libraries(vasdk_entity_test PRIVATE VASDK)
        add_test(NAME vasdk_entity COMMAND vasdk_entity_test)
        add_executable(vasdk_agent_include_test tests/agent_include_test.cpp)
        target_link_libraries(vasdk_agent_include_test PRIVATE VASDK)
        add_test(NAME vasdk_agent_include COMMAND vasdk_agent_include_test)
        add_executable(vasdk_embedding_cache_test tests/embedding_cache_test.cpp)
        target_link_libraries(vasdk_embedding_cache_test PRIVATE VASDK)
        add_test(NAME vasdk_embedding_cache COMMAND vasdk_embedding_cache_test)

        add_executable(vasdk_envelope_layering_test tests/envelope_layering_test.cpp)
        target_link_libraries(vasdk_envelope_layering_test PRIVATE VASDK)
        add_test(NAME vasdk_envelope_layering COMMAND vasdk_envelope_layering_test)

        add_executable(vasdk_envelope_steps_test tests/envelope_steps_test.cpp)
        target_link_libraries(vasdk_envelope_steps_test PRIVATE VASDK)
        add_test(NAME vasdk_envelope_steps COMMAND vasdk_envelope_steps_test)

        add_executable(vasdk_speech_style_test tests/speech_style_test.cpp)
        target_link_libraries(vasdk_speech_style_test PRIVATE VASDK)
        add_test(NAME vasdk_speech_style COMMAND vasdk_speech_style_test)

        add_executable(vasdk_flow_navigation_test tests/flow_navigation_test.cpp)
        target_link_libraries(vasdk_flow_navigation_test PRIVATE VASDK)
        add_test(NAME vasdk_flow_navigation COMMAND vasdk_flow_navigation_test)

        add_executable(vasdk_response_envelope_test tests/response_envelope_test.cpp)
        target_link_libraries(vasdk_response_envelope_test PRIVATE VASDK)
        add_test(NAME vasdk_response_envelope COMMAND vasdk_response_envelope_test)

        add_executable(vasdk_expectation_test tests/expectation_test.cpp)
        target_link_libraries(vasdk_expectation_test PRIVATE VASDK)
        add_test(NAME vasdk_expectation COMMAND vasdk_expectation_test)

        add_executable(vasdk_page_type_test tests/page_type_test.cpp)
        target_link_libraries(vasdk_page_type_test PRIVATE VASDK)
        add_test(NAME vasdk_page_type COMMAND vasdk_page_type_test)

        add_executable(vasdk_primitive_test tests/primitive_test.cpp)
        target_link_libraries(vasdk_primitive_test PRIVATE VASDK)
        add_test(NAME vasdk_primitive COMMAND vasdk_primitive_test)
        add_executable(vasdk_security_test tests/security_test.cpp)
        target_link_libraries(vasdk_security_test PRIVATE VASDK)
        add_test(NAME vasdk_security COMMAND vasdk_security_test)
        add_executable(vasdk_trigger_arbiter_test tests/trigger_arbiter_test.cpp)
        target_link_libraries(vasdk_trigger_arbiter_test PRIVATE VASDK)
        add_test(NAME vasdk_trigger_arbiter COMMAND vasdk_trigger_arbiter_test)
        # The Android app's phone agent ("call mom"), by text on the desktop
        set(PHONE_AGENT ${VASDK_ROOT_DIR}/VASDK_ANDROID/app/src/main/assets/agents/phone/agent.txtpb)
        if(EXISTS ${PHONE_AGENT} AND NOT ANDROID)
            file(GLOB PHONE_AGENT_TESTS ${VASDK_ROOT_DIR}/VASDK_ANDROID/agent-tests/phone/*.txtpb)
            add_test(NAME vasdk_agent_android_phone COMMAND vasdk_agentc test ${PHONE_AGENT} ${PHONE_AGENT_TESTS})
        endif()
        if(TARGET httplib::httplib)
            add_executable(vasdk_remote_control_test tests/remote_control_test.cpp)
            # httplib first: its headers must be the ones VASDK was built with, ahead of another copy
            # on VASDK's include path (a package prefix that holds cpp-httplib too)
            target_link_libraries(vasdk_remote_control_test PRIVATE httplib::httplib VASDK)
            add_test(NAME vasdk_remote_control COMMAND vasdk_remote_control_test)
        endif()
        enable_language(C)
        add_executable(vasdk_c_test tests/vasdk_c_test.c)   # the C API, from C
        target_link_libraries(vasdk_c_test PRIVATE VASDK)
        add_test(NAME vasdk_c COMMAND vasdk_c_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        add_executable(vasdk_model_store_test tests/model_store_test.cpp)   # ModelStore and vasdk_models_*, offline
        target_link_libraries(vasdk_model_store_test PRIVATE VASDK nlohmann_json::nlohmann_json)
        add_test(NAME vasdk_model_store COMMAND vasdk_model_store_test)
        add_test(NAME vasdk_model_store_network COMMAND vasdk_model_store_test --network)
        set_tests_properties(vasdk_model_store_network PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(vasdk_c_session_test tests/c_session_test.cpp)   # spoken sessions through the C API
        target_link_libraries(vasdk_c_session_test PRIVATE VASDK nlohmann_json::nlohmann_json)
        add_test(NAME vasdk_c_session COMMAND vasdk_c_session_test)
        add_executable(vasdk_c_providers_test tests/c_providers_test.cpp)   # providers and processors written in C
        target_link_libraries(vasdk_c_providers_test PRIVATE VASDK nlohmann_json::nlohmann_json)
        add_test(NAME vasdk_c_providers COMMAND vasdk_c_providers_test)
        add_executable(vasdk_c_conversation_test tests/c_conversation_test.cpp)   # conversations and turns, beyond turns in and out
        target_link_libraries(vasdk_c_conversation_test PRIVATE VASDK)
        add_test(NAME vasdk_c_conversation COMMAND vasdk_c_conversation_test)
        set_tests_properties(vasdk_c_providers PROPERTIES TIMEOUT 60)
        add_executable(vasdk_network_fallback_test tests/network_fallback_test.cpp)
        target_link_libraries(vasdk_network_fallback_test PRIVATE VASDK)
        add_test(NAME vasdk_network_fallback COMMAND vasdk_network_fallback_test)
        add_executable(vasdk_llm_assist_test tests/llm_assist_test.cpp)
        target_link_libraries(vasdk_llm_assist_test PRIVATE VASDK)
        add_test(NAME vasdk_llm_assist COMMAND vasdk_llm_assist_test WORKING_DIRECTORY ${VASDK_ROOT_DIR}/..)
        add_executable(vasdk_rule_nlu_test tests/rule_nlu_test.cpp)
        target_link_libraries(vasdk_rule_nlu_test PRIVATE VASDK)
        add_test(NAME vasdk_rule_nlu COMMAND vasdk_rule_nlu_test)
        add_executable(vasdk_entity_extractor_test tests/entity_extractor_test.cpp)
        target_link_libraries(vasdk_entity_extractor_test PRIVATE VASDK)
        add_test(NAME vasdk_entity_extractor COMMAND vasdk_entity_extractor_test)
        add_executable(vasdk_language_test tests/language_test.cpp)
        target_link_libraries(vasdk_language_test PRIVATE VASDK)
        target_compile_definitions(vasdk_language_test PRIVATE VASDK_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
        add_test(NAME vasdk_language COMMAND vasdk_language_test)
        add_executable(vasdk_turn_policy_test tests/turn_policy_test.cpp)
        target_link_libraries(vasdk_turn_policy_test PRIVATE VASDK)
        add_test(NAME vasdk_turn_policy COMMAND vasdk_turn_policy_test)
        add_executable(vasdk_form_edge_cases_test tests/form_edge_cases_test.cpp)
        target_link_libraries(vasdk_form_edge_cases_test PRIVATE VASDK)
        add_test(NAME vasdk_form_edge_cases COMMAND vasdk_form_edge_cases_test)
        add_executable(vasdk_scheduler_test tests/scheduler_test.cpp)
        target_link_libraries(vasdk_scheduler_test PRIVATE VASDK)
        add_test(NAME vasdk_scheduler COMMAND vasdk_scheduler_test)
        # Fuzz targets on seeds and mutations of them; VASDK_BUILD_FUZZERS builds them for libFuzzer
        add_executable(vasdk_fuzz_smoke fuzz/fuzz_targets.cpp fuzz/fuzz_smoke_main.cpp)
        target_link_libraries(vasdk_fuzz_smoke PRIVATE VASDK)
        add_test(NAME vasdk_fuzz_smoke COMMAND vasdk_fuzz_smoke 20000)

        add_executable(vasdk_engine_events_test tests/engine_events_test.cpp)
        target_link_libraries(vasdk_engine_events_test PRIVATE VASDK)
        add_test(NAME vasdk_engine_events COMMAND vasdk_engine_events_test)
    endif()
endif()

if(VASDK_BUILD_TESTS AND NOT WIN32)
    add_executable(vasdk_http_cancel_test tests/http_cancel_test.cpp)
    target_link_libraries(vasdk_http_cancel_test PRIVATE VASDK)
    add_test(NAME vasdk_http_cancel COMMAND vasdk_http_cancel_test)
endif()

# Coverage-guided fuzzers (clang with libFuzzer): vasdk_fuzzAgent, vasdk_fuzzExpression, vasdk_fuzzWords
option(VASDK_BUILD_FUZZERS "Build libFuzzer targets for agent files, expressions and user words" OFF)
if(VASDK_BUILD_FUZZERS AND VASDK_ENABLE_ENGINE_CONFIG)
    foreach(_fuzz_target fuzzAgent fuzzExpression fuzzWords)
        add_executable(vasdk_${_fuzz_target} fuzz/fuzz_targets.cpp)
        target_compile_definitions(vasdk_${_fuzz_target} PRIVATE VASDK_FUZZ_TARGET=${_fuzz_target})
        target_compile_options(vasdk_${_fuzz_target} PRIVATE -fsanitize=fuzzer,address)
        target_link_options(vasdk_${_fuzz_target} PRIVATE -fsanitize=fuzzer,address)
        target_link_libraries(vasdk_${_fuzz_target} PRIVATE VASDK)
    endforeach()
endif()
