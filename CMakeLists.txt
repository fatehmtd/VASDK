cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK_Project VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(FetchContent)

option(VASDK_BUILD_EXAMPLES "Build examples" OFF)
option(VASDK_BUILD_APP "Build VASDK application" ON)
option(VASDK_BUILD_THIRDPARTY_IMPL "Build third-party provider implementations" ON)
option(VASDK_THIRDPARTY_SHARED_LIBS "Build third-party provider libraries as shared (ON) or static (OFF)" OFF)

#  Common dependencies 

option(VASDK_ENABLE_HIGHWAY "Enable Google Highway SIMD acceleration" ON)

if(VASDK_ENABLE_HIGHWAY)
    fetchcontent_declare(
        highway
        GIT_REPOSITORY https://github.com/google/highway.git
        GIT_TAG        "1.2.0"
        GIT_SHALLOW    TRUE
    )
    set(HWY_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_CONTRIB  OFF CACHE BOOL "" FORCE)
    fetchcontent_makeavailable(highway)
endif()

# spdlog (used by VASDK core and providers)
fetchcontent_declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG "v1.15.0"
    OVERRIDE_FIND_PACKAGE
)
fetchcontent_makeavailable(spdlog)

# CLI11 (command-line argument parsing for CLI tools)
fetchcontent_declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        "v2.4.2"
)
fetchcontent_makeavailable(CLI11)

# nlohmann_json (used by VASDK core, gladiapp, deepgrampp, supertonic)
fetchcontent_declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3  # pinned for reproducible builds
    OVERRIDE_FIND_PACKAGE
)
fetchcontent_makeavailable(nlohmann_json)

# dotenv-cpp (loads .env files into process environment at startup)
fetchcontent_declare(
    dotenv_cpp
    GIT_REPOSITORY https://github.com/laserpants/dotenv-cpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
set(BUILD_DOCS  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
fetchcontent_makeavailable(dotenv_cpp)

# FTXUI (terminal UI - optional, enabled by VASDK_BUILD_TUI)
option(VASDK_BUILD_TUI "Build FTXUI terminal UI for the pipeline app" OFF)
if(VASDK_BUILD_TUI)
    fetchcontent_declare(
        ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        "v6.0.2"
        GIT_SHALLOW    TRUE
    )
    set(FTXUI_BUILD_DOCS    OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    fetchcontent_makeavailable(ftxui)
endif()

# ctrack - performance profiling (ON by default)
option(VASDK_ENABLE_CTRACK "Enable ctrack performance profiling" ON)
if(VASDK_ENABLE_CTRACK)
    fetchcontent_declare(
        ctrack
        GIT_REPOSITORY https://github.com/Compaile/ctrack.git
        GIT_TAG        v1.1.0
        GIT_SHALLOW    ON
    )
    set(DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
    set(DISABLE_TESTS    ON CACHE BOOL "" FORCE)
    set(DISABLE_PAR      ON CACHE BOOL "" FORCE)
    fetchcontent_makeavailable(ctrack)

    add_library(vasdk_ctrack_iface INTERFACE)
    target_include_directories(vasdk_ctrack_iface INTERFACE ${ctrack_SOURCE_DIR}/include)
    target_compile_definitions(vasdk_ctrack_iface INTERFACE
        VASDK_ENABLE_CTRACK
        CTRACK_DISABLE_EXECUTION_POLICY  # std::execution::par_unseq unavailable without TBB on Apple/Linux
    )
endif()

#  Core library 

add_subdirectory(VASDK)

#  Third-party provider implementations 

if(VASDK_BUILD_THIRDPARTY_IMPL)
    add_subdirectory(VASDK_3P_IMPL)
endif()

#  Application 

if(VASDK_BUILD_APP)
    add_subdirectory(VASDK_APP)
endif()

#  Call center voice AI showcase

option(VASDK_BUILD_CALLCENTER "Build goal-oriented call center pipeline showcase" ON)
if(VASDK_BUILD_CALLCENTER)
    add_subdirectory(VASDK_CALLCENTER_APP)
endif()
