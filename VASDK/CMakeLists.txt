cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#  Metrics feature options 
option(VASDK_METRICS_PROMETHEUS "Build Prometheus /metrics HTTP exporter"      OFF)
option(VASDK_METRICS_STATSD     "Build StatsD UDP metrics transport"           OFF)
option(VASDK_METRICS_INFLUX     "Build InfluxDB v2 Line Protocol transport"    OFF)
option(VASDK_METRICS_WEBSOCKET  "Build WebSocket metrics push transport"       OFF)

#  RequestFlow orchestrator module (va::orchestrator)
# See docs/reference/requestflow-spec.md and docs/reference/requestflow-roadmap.md
# (Milestone 0). Default OFF: this module is new/unproven and pulls protobuf as a
# build-from-source dependency, unlike everything else in this file.
option(VASDK_BUILD_ORCHESTRATOR "Build the RequestFlow orchestrator module (va::orchestrator)" OFF)


add_library(VASDK SHARED
    # Core sources
    ./src/AudioRingBuffer.cpp
    ./src/AudioSIMD.cpp
    ./src/VAEngine.cpp
    # Transport infrastructure
    ./src/Transport.cpp
    ./src/HttpTransport.cpp
    ./src/LocalTransport.cpp
    ./src/WebSocketTransport.cpp
    ./src/MockTransport.cpp
    ./src/TransportFactory.cpp
    # LLM infrastructure (va::llm namespace)
    ./src/llm/LLMTransport.cpp
    ./src/llm/OpenAITransport.cpp
    ./src/llm/AnthropicTransport.cpp
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
    ./src/llm/LLMProcessor.cpp
    ./src/llm/AggregatingLLMProcessor.cpp
    ./src/llm/ToolDispatchProcessor.cpp
    ./src/AudioDSPProcessor.cpp
    ./src/Pipeline.cpp
    ./src/TurnDetectionProcessor.cpp
    ./src/AgentStateProcessor.cpp
    # Goal-oriented call center framework (va::goal namespace)
    ./src/goal/GoalState.cpp
    ./src/goal/GoalManager.cpp
    ./src/goal/IntentRouter.cpp
    ./src/goal/GoalStatePersistence.cpp
    ./src/goal/ConditionEvaluator.cpp
    ./src/goal/GoalDefinitionLoader.cpp
    ./src/goal/GoalConversationSimulator.cpp
    ./src/SessionManager.cpp
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

target_link_libraries(VASDK
    PUBLIC spdlog::spdlog
)

#  Optional metrics exporters 
if(VASDK_METRICS_STATSD)
    target_sources(VASDK PRIVATE ./src/metrics/StatsDMetricsTransport.cpp)
    target_compile_definitions(VASDK PUBLIC VASDK_METRICS_STATSD)
    message(STATUS "VASDK metrics: StatsD UDP exporter enabled")
endif()

if(VASDK_METRICS_INFLUX)
    target_sources(VASDK PRIVATE ./src/metrics/InfluxMetricsTransport.cpp)
    target_compile_definitions(VASDK PUBLIC VASDK_METRICS_INFLUX)
    find_package(CURL REQUIRED)
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
if(VASDK_ENABLE_HIGHWAY AND TARGET hwy)
    target_link_libraries(VASDK PRIVATE hwy)
    target_compile_definitions(VASDK PRIVATE VASDK_HIGHWAY_ENABLED)
    # On x86_64 desktop, enable AVX2+FMA so Highway picks a wider ISA than SSE2
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT ANDROID)
        target_compile_options(VASDK PRIVATE -mavx2 -mfma)
    endif()
    message(STATUS "VASDK: Google Highway SIMD acceleration enabled")
else()
    message(STATUS "VASDK: Google Highway not available - using scalar audio helpers")
endif()

# Optional: Link nlohmann_json if available (for WebSocketTransport and LLMTransport serialization)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(VASDK PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_JSON)
    message(STATUS "nlohmann_json found - WebSocketTransport and LLMTransport will use JSON serialization")
else()
    message(STATUS "nlohmann_json not found - WebSocketTransport and LLMTransport will use stub serialization")
endif()

#  cpp-httplib (shared between Prometheus exporter and WebSocket server) 
option(VASDK_ENABLE_WEBSOCKET_SERVER "Enable WebSocket server transport via cpp-httplib" ON)

if(VASDK_METRICS_PROMETHEUS OR VASDK_ENABLE_WEBSOCKET_SERVER)
    if(NOT TARGET httplib::httplib)
        include(FetchContent)
        fetchcontent_declare(
            cpp-httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG        v0.18.3
        )
        fetchcontent_makeavailable(cpp-httplib)
    endif()
    target_link_libraries(VASDK PRIVATE httplib::httplib)
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_HTTPLIB)
    message(STATUS "VASDK: cpp-httplib available (Prometheus=${VASDK_METRICS_PROMETHEUS}, WebSocket=${VASDK_ENABLE_WEBSOCKET_SERVER})")
endif()

if(VASDK_ENABLE_WEBSOCKET_SERVER)
    target_sources(VASDK PRIVATE ./src/network/CppHttpLibWebSocketProvider.cpp)
    message(STATUS "VASDK: WebSocket server provider enabled")
endif()

# Find and link libcurl for HTTP transports (optional - disable with -DVASDK_ENABLE_HTTP_TRANSPORT=OFF)
option(VASDK_ENABLE_HTTP_TRANSPORT "Enable HTTP-based transports (requires libcurl)" ON)
if(VASDK_ENABLE_HTTP_TRANSPORT)
    if(NOT TARGET CURL::libcurl)
        find_package(CURL REQUIRED)
    endif()
    target_link_libraries(VASDK PRIVATE CURL::libcurl)
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_CURL)
    target_sources(VASDK PRIVATE ./src/network/CurlHttpProvider.cpp)
    message(STATUS "libcurl found - HTTP transport support enabled")
else()
    message(STATUS "VASDK_ENABLE_HTTP_TRANSPORT=OFF - skipping libcurl, HTTP transports will stub out")
endif()

#  RequestFlow orchestrator module (va::orchestrator)
# cel-cpp is deliberately NOT wired here - it has no CMake build of its own
# (Bazel-only, confirmed against its repo: no CMakeLists.txt, no vcpkg/Conan
# package). Resolving that is a tracked open decision ahead of Milestone 1.3
# (see docs/reference/requestflow-roadmap.md); Milestone 0 only proves the
# module skeleton and the one dependency (protobuf) that does have real CMake
# support.
if(VASDK_BUILD_ORCHESTRATOR)
    fetchcontent_declare(
        protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG        v36.1
        GIT_SHALLOW    TRUE
    )
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL     OFF CACHE BOOL "" FORCE)
    fetchcontent_makeavailable(protobuf)
    # Observed on this dev machine: protobuf's own CMake resolved its Abseil
    # dependency via find_package() against a Homebrew-installed abseil
    # (/opt/homebrew/opt/abseil), not via FetchContent. Unverified whether
    # protobuf_ABSL_PROVIDER falls back to fetching its own copy when no
    # system abseil is present (e.g. on a clean CI machine) - worth confirming
    # before relying on this option in CI.

    # M1.1 (requestflow-roadmap.md §3) - the .proto schema from requestflow-spec.md §4.
    # Invokes protobuf::protoc directly rather than the protobuf_generate() CMake
    # helper - that helper is NOT reliably registered by fetchcontent_makeavailable()
    # (confirmed: "Unknown CMake command protobuf_generate" when building from
    # source this way), so this is the verified-working path, not the documented one.
    # The second -I (protobuf's own src/) lets `import "google/protobuf/struct.proto"`
    # etc. resolve against the well-known-type .proto sources FetchContent just pulled.
    #
    # M1.3 (requestflow-roadmap.md §3) added proto/orchestrator/training.proto (spec §22)
    # alongside domain.proto - looped rather than duplicated per-file below.
    set(VASDK_ORCHESTRATOR_PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${VASDK_ORCHESTRATOR_PROTO_OUT_DIR}/orchestrator)
    set(VASDK_ORCHESTRATOR_PROTO_STEMS domain training)
    set(VASDK_ORCHESTRATOR_PROTO_SRCS)   # truly empty list - set(... "") would leave a leading empty-string element
    foreach(_vasdk_orch_proto_stem ${VASDK_ORCHESTRATOR_PROTO_STEMS})
        set(_vasdk_orch_proto ${CMAKE_CURRENT_SOURCE_DIR}/proto/orchestrator/${_vasdk_orch_proto_stem}.proto)
        set(_vasdk_orch_proto_cc ${VASDK_ORCHESTRATOR_PROTO_OUT_DIR}/orchestrator/${_vasdk_orch_proto_stem}.pb.cc)
        set(_vasdk_orch_proto_h  ${VASDK_ORCHESTRATOR_PROTO_OUT_DIR}/orchestrator/${_vasdk_orch_proto_stem}.pb.h)
        add_custom_command(
            OUTPUT  ${_vasdk_orch_proto_cc} ${_vasdk_orch_proto_h}
            COMMAND protobuf::protoc
            ARGS    --cpp_out=${VASDK_ORCHESTRATOR_PROTO_OUT_DIR}
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
                    -I ${protobuf_SOURCE_DIR}/src
                    ${_vasdk_orch_proto}
            DEPENDS ${_vasdk_orch_proto} protobuf::protoc
            COMMENT "Generating C++ protobuf sources for va::orchestrator ${_vasdk_orch_proto_stem} schema"
            VERBATIM
        )
        list(APPEND VASDK_ORCHESTRATOR_PROTO_SRCS ${_vasdk_orch_proto_cc} ${_vasdk_orch_proto_h})
    endforeach()

    target_sources(VASDK PRIVATE
        ./src/orchestrator/OrchestratorModule.cpp
        ./src/orchestrator/OrchestratorConfigLoader.cpp
        ./src/orchestrator/PhraseGrammarMatcher.cpp
        ${VASDK_ORCHESTRATOR_PROTO_SRCS}
    )
    target_include_directories(VASDK PRIVATE ${VASDK_ORCHESTRATOR_PROTO_OUT_DIR})
    target_link_libraries(VASDK PRIVATE protobuf::libprotobuf)
    target_compile_definitions(VASDK PRIVATE VASDK_HAS_ORCHESTRATOR)
    message(STATUS "VASDK: orchestrator module enabled (va::orchestrator) - protobuf + domain.proto wired, cel-cpp not yet (see requestflow-roadmap.md)")
else()
    message(STATUS "VASDK: orchestrator module disabled (VASDK_BUILD_ORCHESTRATOR=OFF)")
endif()

set_target_properties(VASDK PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

include(GNUInstallDirs)
install(TARGETS VASDK
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Generate export header
include(GenerateExportHeader)
generate_export_header(VASDK
    BASE_NAME VASDK
    EXPORT_FILE_NAME vasdk_export.h
)

target_include_directories(VASDK PUBLIC
    $<INSTALL_INTERFACE:include>  # For installed package (optional but good practice)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> # For direct build
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}> # To export the symbols header
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/VASDK # to simplify the internal include files
)

if(VASDK_ENABLE_CTRACK AND TARGET vasdk_ctrack_iface)
    target_link_libraries(VASDK PRIVATE vasdk_ctrack_iface)
endif()

# Install headers
install(DIRECTORY include/VASDK
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Install export header
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VASDK
)
