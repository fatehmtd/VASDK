cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK LANGUAGES CXX)

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
# One protobuf per binary: with a static protobuf (built from source, as on Android), VASDK is
# static too, so it and the app's own generated code (provider options, extensions) share the one
# copy linked into the app. Two copies would keep two registries of message types.
set(VASDK_LIBRARY_TYPE SHARED)
if(TARGET protobuf::libprotobuf)
    get_target_property(_protobuf_type protobuf::libprotobuf TYPE)
    if(_protobuf_type STREQUAL "STATIC_LIBRARY")
        set(VASDK_LIBRARY_TYPE STATIC)
    endif()
endif()

add_library(VASDK ${VASDK_LIBRARY_TYPE}
    # Core sources
    ./src/AudioRingBuffer.cpp
    ./src/AudioSIMD.cpp
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
    ./src/llm/LLMProcessor.cpp
    ./src/llm/AggregatingLLMProcessor.cpp
    ./src/llm/ToolDispatchProcessor.cpp
    ./src/AudioDSPProcessor.cpp
    ./src/Pipeline.cpp
    ./src/TurnDetectionProcessor.cpp
    ./src/TurnAssemblerProcessor.cpp
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
    target_link_libraries(VASDK PUBLIC nlohmann_json::nlohmann_json)   # public headers (llm/Tool.h, flow/Tools.h) use it
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
        ./src/nlu/English.cpp
        ./proto/vasdk/engine/engine_config.proto
        ./proto/vasdk/engine/events.proto
        ./proto/vasdk/nlu/entity.proto
        ./proto/vasdk/flow/expectation.proto
        ./proto/vasdk/flow/trigger.proto
        ./proto/vasdk/goal/goal.proto
        ./proto/vasdk/flow/agent.proto
        ./proto/vasdk/flow/page_types.proto
        ./proto/vasdk/flow/test_case.proto
        ./proto/vasdk/flow/step_outcome.proto
    )
    protobuf_generate(TARGET VASDK
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
    target_include_directories(VASDK PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/proto>)
    target_link_libraries(VASDK PUBLIC protobuf::libprotobuf)
    target_compile_definitions(VASDK PUBLIC VASDK_HAS_ENGINE_CONFIG)
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

        add_executable(vasdk_expression_test tests/expression_test.cpp)
        target_link_libraries(vasdk_expression_test PRIVATE VASDK)
        add_test(NAME vasdk_expression COMMAND vasdk_expression_test)

        add_executable(vasdk_dialog_test tests/dialog_test.cpp)
        target_link_libraries(vasdk_dialog_test PRIVATE VASDK nlohmann_json::nlohmann_json)
        add_test(NAME vasdk_dialog COMMAND vasdk_dialog_test)

        add_executable(vasdk_form_primitives_test tests/form_primitives_test.cpp)
        target_link_libraries(vasdk_form_primitives_test PRIVATE VASDK)
        add_test(NAME vasdk_form_primitives COMMAND vasdk_form_primitives_test)

        add_executable(vasdk_entity_test tests/entity_test.cpp)
        target_link_libraries(vasdk_entity_test PRIVATE VASDK)
        add_test(NAME vasdk_entity COMMAND vasdk_entity_test)

        add_executable(vasdk_envelope_layering_test tests/envelope_layering_test.cpp)
        target_link_libraries(vasdk_envelope_layering_test PRIVATE VASDK)
        add_test(NAME vasdk_envelope_layering COMMAND vasdk_envelope_layering_test)

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
        add_executable(vasdk_rule_nlu_test tests/rule_nlu_test.cpp)
        target_link_libraries(vasdk_rule_nlu_test PRIVATE VASDK)
        add_test(NAME vasdk_rule_nlu COMMAND vasdk_rule_nlu_test)
        add_executable(vasdk_entity_extractor_test tests/entity_extractor_test.cpp)
        target_link_libraries(vasdk_entity_extractor_test PRIVATE VASDK)
        add_test(NAME vasdk_entity_extractor COMMAND vasdk_entity_extractor_test)
        add_executable(vasdk_turn_policy_test tests/turn_policy_test.cpp)
        target_link_libraries(vasdk_turn_policy_test PRIVATE VASDK)
        add_test(NAME vasdk_turn_policy COMMAND vasdk_turn_policy_test)
        add_executable(vasdk_form_edge_cases_test tests/form_edge_cases_test.cpp)
        target_link_libraries(vasdk_form_edge_cases_test PRIVATE VASDK)
        add_test(NAME vasdk_form_edge_cases COMMAND vasdk_form_edge_cases_test)

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
