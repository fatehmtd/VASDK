cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK_CAR_APP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# The car, and car_agentc: vasdk_agentc with its tools registered, which runs the agent's scripted
# drives. Needs no audio, so before the checks below.
if(NOT (VASDK_ENABLE_ENGINE_CONFIG AND TARGET VASDK))
    message(STATUS "[VASDK_Car] skipped: needs VASDK_ENABLE_ENGINE_CONFIG")
    return()
endif()

# The car: the core (core/) and every vertical (verticals/<name>/), each with its code, its part of
# the agent, its tests and its dashboard panel. A vertical with code needs its folder here and a
# line in core/Verticals.cpp; one with none (manual) is only its agent file.
set(CAR_VERTICALS drive climate media phone messages navigation profiles)
set(CAR_AGENT ${CMAKE_CURRENT_SOURCE_DIR}/agents/car/agent.txtpb)

set(CAR_SOURCES core/Car.cpp core/Verticals.cpp)
set(CAR_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/core)
foreach(vertical ${CAR_VERTICALS})
    file(GLOB vertical_sources ${CMAKE_CURRENT_SOURCE_DIR}/verticals/${vertical}/*.cpp)
    list(FILTER vertical_sources EXCLUDE REGEX "_test\\.cpp$")
    list(APPEND CAR_SOURCES ${vertical_sources})
    list(APPEND CAR_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/verticals/${vertical})
endforeach()
add_library(car_core STATIC ${CAR_SOURCES})
target_include_directories(car_core PUBLIC ${CAR_INCLUDES})
target_link_libraries(car_core PUBLIC VASDK)

add_executable(car_agentc core/car_agentc.cpp)
target_link_libraries(car_agentc PRIVATE car_core)

if(VASDK_BUILD_TESTS)
    # Each vertical's conversations, by text (vasdk_car_<name>), and its code against the agent
    # (vasdk_car_<name>_code, from verticals/<name>/<name>_test.cpp); the shared parts' in agents/car/tests.
    file(GLOB CAR_PARTS LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/verticals/*)
    list(APPEND CAR_PARTS ${CMAKE_CURRENT_SOURCE_DIR}/agents/car)
    foreach(part ${CAR_PARTS})
        get_filename_component(part_name ${part} NAME)
        if(part_name STREQUAL "car")
            set(part_name shared)   # agents/car: what every vertical shares
        endif()
        file(GLOB part_cases ${part}/tests/*.txtpb)
        if(part_cases)
            add_test(NAME vasdk_car_${part_name} COMMAND car_agentc test ${CAR_AGENT} ${part_cases})
        endif()
        if(EXISTS ${part}/${part_name}_test.cpp)
            add_executable(car_${part_name}_test ${part}/${part_name}_test.cpp)
            target_link_libraries(car_${part_name}_test PRIVATE car_core)
            add_test(NAME vasdk_car_${part_name}_code COMMAND car_${part_name}_test ${CAR_AGENT})
            list(APPEND CAR_TEST_TOOLS car_${part_name}_test)
        endif()
    endforeach()
endif()

if(WIN32)   # a VASDK.dll beside them is loaded first: keep it the current one
    get_target_property(_vasdk_type VASDK TYPE)
    if(_vasdk_type STREQUAL "SHARED_LIBRARY")
        foreach(_tool car_agentc ${CAR_TEST_TOOLS})
            add_custom_command(TARGET ${_tool} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:VASDK> $<TARGET_FILE_DIR:${_tool}>)
        endforeach()
    endif()
endif()

#  The app: needs audio I/O, on-device speech and an HTTP server for the dashboard

set(CAR_MISSING "")
if(NOT VASDK_BUILD_SDL AND NOT VASDK_BUILD_MINIAUDIO)
    list(APPEND CAR_MISSING "SDL or MINIAUDIO (audio I/O)")
endif()
if(NOT VASDK_BUILD_SILERO_VAD)
    list(APPEND CAR_MISSING "SILERO_VAD")
endif()
if(NOT TARGET vasdk_provider_catalog)
    list(APPEND CAR_MISSING "the provider catalog (VASDK_BUILD_THIRDPARTY_IMPL)")
endif()
if(NOT TARGET httplib::httplib)
    list(APPEND CAR_MISSING "cpp-httplib (VASDK_ENABLE_WEBSOCKET_SERVER or VASDK_METRICS_PROMETHEUS)")
endif()
if(CAR_MISSING)
    message(WARNING "VASDK_Car needs: ${CAR_MISSING}. Skipping it; car_agentc and the tests are still built.")
    return()
endif()

add_executable(VASDK_Car car_pipeline.cpp dashboard/DashboardServer.cpp)
target_include_directories(VASDK_Car PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dashboard)
target_link_libraries(VASDK_Car PRIVATE
    VASDK car_core vasdk_provider_catalog httplib::httplib
    nlohmann_json::nlohmann_json CLI11::CLI11 spdlog::spdlog dotenv)
# Where the config, agent, prompts and dashboard page are found by default
target_compile_definitions(VASDK_Car PRIVATE CAR_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(VASDK_BUILD_MINIAUDIO AND TARGET vasdk_miniaudio_audio)
    target_link_libraries(VASDK_Car PRIVATE vasdk_miniaudio_audio)
    target_compile_definitions(VASDK_Car PRIVATE VASDK_HAS_MINIAUDIO)
elseif(VASDK_BUILD_SDL AND TARGET vasdk_sdl_audio)
    target_link_libraries(VASDK_Car PRIVATE vasdk_sdl_audio)
    target_compile_definitions(VASDK_Car PRIVATE VASDK_HAS_SDL)
endif()

if(VASDK_BUILD_LIBDATACHANNEL_WEBRTC AND TARGET vasdk_libdatachannel_webrtc)
    target_link_libraries(VASDK_Car PRIVATE vasdk_libdatachannel_webrtc)
    target_compile_definitions(VASDK_Car PRIVATE VASDK_HAS_LIBDATACHANNEL_WEBRTC)   # --webrtc: talk from the dashboard
endif()

if(WIN32)
    add_custom_command(TARGET VASDK_Car POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll" $<TARGET_FILE_DIR:VASDK_Car>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${onnxruntime_SOURCE_DIR}/lib/onnxruntime_providers_shared.dll" $<TARGET_FILE_DIR:VASDK_Car>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:VASDK> $<TARGET_FILE_DIR:VASDK_Car>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:VASDK_Car> $<TARGET_FILE_DIR:VASDK_Car>
        COMMAND_EXPAND_LISTS)
    if(VASDK_BUILD_SDL AND TARGET SDL3::SDL3)
        add_custom_command(TARGET VASDK_Car POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:VASDK_Car>)
    endif()
    if(VASDK_ENABLE_HTTP_TRANSPORT AND NOT VASDK_CURL_BUNDLED)
        add_custom_command(TARGET VASDK_Car POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,libcurl-d.dll,libcurl.dll>" $<TARGET_FILE_DIR:VASDK_Car>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,zlibd1.dll,zlib1.dll>" $<TARGET_FILE_DIR:VASDK_Car>)
    endif()
endif()

install(TARGETS VASDK_Car RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
