cmake_minimum_required(VERSION 3.21...4.0)

# The one version of the SDK, the C library and every binding: the VERSION file (docs/releasing.md)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION VASDK_VERSION LIMIT_COUNT 1)
project(VASDK_Project VERSION ${VASDK_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# MSVC: the C library loads into processes that bring an older C++ runtime (msvcp140.dll) of their
# own: a JVM, a game engine. Since Visual Studio 17.10 a std::mutex built here crashes on lock
# there; this keeps the constructor that works with every msvcp140. Here, before the dependencies
# are added, so what is fetched and linked into vasdk_c (spdlog...) gets it too.
add_compile_definitions($<$<CXX_COMPILER_ID:MSVC>:_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR>)

# The repository's root, wherever VASDK sits: on its own, or inside another project by
# add_subdirectory or FetchContent (docs/integrating.md). Never CMAKE_SOURCE_DIR: that's the outer
# project's.
set(VASDK_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(VASDK_ROOT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(GNUInstallDirs)
include(FetchContent)

# curl's own CMakeLists.txt resolves libidn2 via pkg-config and calls
# link_directories() in its own directory scope. That directory property isn't
# inherited by sibling directories like VASDK_APP/VASDK_CALLCENTER_APP, so their
# final executable link sees a bare "-lidn2" with no matching "-L" and fails.
# Locate it via CMake's own find_library (searches standard system paths
# regardless of package manager) and add its directory globally so every
# subdirectory inherits it.
find_library(VASDK_LIBIDN2_LIBRARY NAMES idn2 libidn2)
if(VASDK_LIBIDN2_LIBRARY)
    get_filename_component(VASDK_LIBIDN2_LIBDIR "${VASDK_LIBIDN2_LIBRARY}" DIRECTORY)
    link_directories(${VASDK_LIBIDN2_LIBDIR})
endif()

option(VASDK_BUILD_EXAMPLES "Build examples" OFF)
# The sample apps are optional: a default build is the SDK alone (docs/apps). VASDK_BUILD_APPS
# turns on all three; each has its own switch too.
option(VASDK_BUILD_APPS "Build every sample app (pipeline, call center, car)" OFF)
option(VASDK_BUILD_APP "Build the pipeline sample app (VASDK_APP)" OFF)
option(VASDK_BUILD_THIRDPARTY_IMPL "Build third-party provider implementations" ON)
option(VASDK_THIRDPARTY_SHARED_LIBS "Build third-party provider libraries as shared (ON) or static (OFF)" OFF)
option(VASDK_BUILD_TESTS "Build unit tests (run with ctest)" OFF)
# The C library (VASDK::c): the C API and the providers in one shared library with nothing but
# vasdk_* exported. What the bindings load and what the release archives and packages ship.
option(VASDK_BUILD_C_LIBRARY "Build the C library, vasdk_c (VASDK::c)" ON)
# The language bindings over the C library, off by default
option(VASDK_BUILD_PYTHON_BINDING "Python binding (bindings/python): its tests and the Python app's" OFF)
option(VASDK_BUILD_KOTLIN_BINDING "Kotlin binding (bindings/kotlin): the JVM library and its tests" OFF)
# Models are fetched at configure time for a source build; a project that embeds VASDK, and every
# package, gets them at run time instead (docs/integrating.md)
option(VASDK_DOWNLOAD_MODELS "Download the enabled providers' models at configure time" ${PROJECT_IS_TOP_LEVEL})
option(VASDK_INSTALL "Generate install rules (find_package(VASDK), pkg-config)" ON)
if(VASDK_BUILD_TESTS)
    enable_testing()
endif()

#  Common dependencies 

include(VASDKDependencies)

option(VASDK_ENABLE_HIGHWAY "Enable Google Highway SIMD acceleration" ON)

if(VASDK_ENABLE_HIGHWAY)
    set(HWY_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_CONTRIB  OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
    vasdk_dependency(hwy TARGET hwy::hwy
        FIND CONFIG
        DECLARE GIT_REPOSITORY https://github.com/google/highway.git
                GIT_TAG        "1.2.0"
                GIT_SHALLOW    TRUE)
    if(NOT TARGET hwy::hwy AND TARGET hwy)
        add_library(hwy::hwy ALIAS hwy)
    endif()
endif()

# spdlog (used by VASDK core and providers)
vasdk_dependency(spdlog TARGET spdlog::spdlog
    FIND CONFIG
    DECLARE GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG "v1.15.0"
            OVERRIDE_FIND_PACKAGE)

# nlohmann_json (used by VASDK core, gladiapp, deepgrampp, supertonic)
vasdk_dependency(nlohmann_json TARGET nlohmann_json::nlohmann_json
    FIND CONFIG
    DECLARE GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3  # pinned for reproducible builds
            OVERRIDE_FIND_PACKAGE)

# The sample apps' own: CLI11 (command-line arguments) and dotenv-cpp (.env files into the environment)
if(VASDK_BUILD_APP OR VASDK_BUILD_APPS OR VASDK_BUILD_CALLCENTER OR VASDK_BUILD_CAR)
    vasdk_dependency(CLI11 TARGET CLI11::CLI11
        FIND CONFIG
        DECLARE GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
                GIT_TAG        "v2.4.2")
    set(BUILD_DOCS  OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        dotenv_cpp
        GIT_REPOSITORY https://github.com/laserpants/dotenv-cpp.git
        GIT_TAG        9275210b8abf1a551fb81e0ba45866286d764acd   # master, pinned
        ${VASDK_FETCH_EXCLUDE}   # its own install rules stay out of VASDK's
    )
    fetchcontent_makeavailable(dotenv_cpp)
endif()

# FTXUI (terminal UI - optional, enabled by VASDK_BUILD_TUI)
option(VASDK_BUILD_TUI "Build FTXUI terminal UI for the pipeline app" OFF)
if(VASDK_BUILD_TUI)
    fetchcontent_declare(
        ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        "v6.0.2"
        GIT_SHALLOW    TRUE
        ${VASDK_FETCH_EXCLUDE}   # its own install rules stay out of VASDK's
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
        ${VASDK_FETCH_EXCLUDE}   # its own install rules stay out of VASDK's
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

#  Third-party provider implementations: which ones, by flavor (core, default, full) or one by one

include(VASDKFlavors)

if(VASDK_BUILD_THIRDPARTY_IMPL)
    add_subdirectory(VASDK_3P_IMPL)
endif()

#  The C library: vasdk_c (VASDK::c), the C API and every provider built in

if(VASDK_BUILD_C_LIBRARY)
    add_subdirectory(cmake/c_library)
endif()

#  The sample apps, optional (VASDK_BUILD_APPS, or each one's switch). Never installed by default:
#  cmake --install <build> --component apps

option(VASDK_BUILD_CALLCENTER "Build the goal-oriented call center sample app" OFF)
option(VASDK_BUILD_CAR "Build the in-car assistant sample app" OFF)

if(VASDK_BUILD_APP OR VASDK_BUILD_APPS)
    add_subdirectory(VASDK_APP)
endif()

#  Call center voice AI showcase

if(VASDK_BUILD_CALLCENTER OR VASDK_BUILD_APPS)
    add_subdirectory(VASDK_CALLCENTER_APP)
endif()

#  In-car assistant showcase: climate, media and phone, with a live dashboard

if(VASDK_BUILD_CAR OR VASDK_BUILD_APPS)
    add_subdirectory(VASDK_CAR_APP)
endif()

#  The bindings load the C library at run time

if((VASDK_BUILD_PYTHON_BINDING OR VASDK_BUILD_KOTLIN_BINDING) AND NOT TARGET vasdk_c)
    message(FATAL_ERROR "VASDK_BUILD_PYTHON_BINDING and VASDK_BUILD_KOTLIN_BINDING load the C library: "
                        "turn on VASDK_BUILD_C_LIBRARY and VASDK_ENABLE_ENGINE_CONFIG")
endif()

#  Kitchen assistant in Python, on the C API's binding: tests only (the app is Python)

if(VASDK_BUILD_TESTS AND VASDK_BUILD_PYTHON_BINDING)
    add_subdirectory(VASDK_PYTHON_APP)
endif()

#  The Kotlin binding on the JVM (bindings/kotlin), by Gradle; Android builds the same sources (VASDK_ANDROID/vasdk)

if(VASDK_BUILD_KOTLIN_BINDING)
    add_subdirectory(bindings/kotlin)
endif()

#  The agent skills (skills/, AGENTS.md): every path, link, heading and provider id they name
#  still exists, so a rename fails here instead of leaving a skill stale. And the third-party code
#  rules (va::thirdparty::<category>, no static variables) on every integration in VASDK_3P_IMPL

if(VASDK_BUILD_TESTS)
    # The configure-time model downloads (VASDK_3P_IMPL/cmake/VASDKModels.cmake): offline, and every
    # model id the build can ask for is in the manifest
    add_test(NAME vasdk_model_downloads
             COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/model_downloads_test
                     -P ${VASDK_ROOT_DIR}/VASDK_3P_IMPL/cmake/test_model_downloads.cmake)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME vasdk_skills COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/skills/check_skills.py)
        add_test(NAME vasdk_skills_self_test COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/skills/check_skills.py --self-test)
        add_test(NAME vasdk_thirdparty_rules COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/VASDK_3P_IMPL/check_rules.py)
        add_test(NAME vasdk_thirdparty_rules_self_test COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/VASDK_3P_IMPL/check_rules.py --self-test)
        # Every place stating the version agrees with the VERSION file (docs/releasing.md)
        add_test(NAME vasdk_versions COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/cmake/check_versions.py)
        # The model store from Python (the other Python suites run the sample apps' agents: VASDK_APP)
        if(VASDK_BUILD_PYTHON_BINDING)
            add_test(NAME vasdk_python_models COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/bindings/python/tests/test_models.py)
            set_tests_properties(vasdk_python_models PROPERTIES ENVIRONMENT "VASDK_LIBRARY=$<TARGET_FILE:vasdk_c>")
        endif()
        # C and the bindings that are on level with the C++ API (skills/vasdk-binding-parity)
        if(VASDK_BUILD_PYTHON_BINDING OR VASDK_BUILD_KOTLIN_BINDING)
            set(_vasdk_parity_bindings c)
            if(VASDK_BUILD_PYTHON_BINDING)
                string(APPEND _vasdk_parity_bindings ",python")
            endif()
            if(VASDK_BUILD_KOTLIN_BINDING)
                string(APPEND _vasdk_parity_bindings ",kotlin")
            endif()
            add_test(NAME vasdk_binding_parity
                     COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/skills/vasdk-binding-parity/check_parity.py
                             --bindings ${_vasdk_parity_bindings})
            add_test(NAME vasdk_binding_parity_self_test
                     COMMAND ${Python3_EXECUTABLE} ${VASDK_ROOT_DIR}/skills/vasdk-binding-parity/check_parity.py --self-test)
        else()
            message(WARNING "VASDK_BUILD_PYTHON_BINDING and VASDK_BUILD_KOTLIN_BINDING are off: the binding parity "
                            "checks (vasdk_binding_parity) are not registered, so a C++ API the bindings don't follow "
                            "goes unnoticed. Turn a binding on, or run skills/vasdk-binding-parity/check_parity.py")
        endif()
        # The binding's tests (vasdk_python_*) need protobuf in this interpreter
        if(VASDK_BUILD_PYTHON_BINDING)
            execute_process(COMMAND ${Python3_EXECUTABLE} -c "import google.protobuf"
                            RESULT_VARIABLE _vasdk_python_protobuf OUTPUT_QUIET ERROR_QUIET)
            if(NOT _vasdk_python_protobuf EQUAL 0)
                message(WARNING "${Python3_EXECUTABLE} has no protobuf: the vasdk_python_* tests will fail. "
                                "Run `uv sync --project VASDK_PYTHON_APP` and configure with "
                                "-DPython3_EXECUTABLE=VASDK_PYTHON_APP/.venv/bin/python (Windows: .venv/Scripts/python.exe), "
                                "or `pip install protobuf` (docs/apps/python.md)")
            endif()
        endif()
    else()
        message(WARNING "Python 3 not found: the vasdk_skills and vasdk_thirdparty_rules tests are not registered")
    endif()
endif()

#  find_package(VASDK), pkg-config and the version header's install (docs/integrating.md)

if(VASDK_INSTALL)
    include(VASDKInstall)
endif()

#  Another project's view: a C program built against this build's install prefix and its build
#  tree with find_package(VASDK) (tests/consumer)

if(VASDK_BUILD_TESTS AND VASDK_INSTALL AND TARGET vasdk_c)
    foreach(_mode install build_tree)
        add_test(NAME vasdk_consumer_${_mode}
                 COMMAND ${CMAKE_COMMAND} -DMODE=${_mode} -DVASDK_BUILD_DIR=${VASDK_ROOT_BINARY_DIR} -DCONFIG=$<CONFIG>
                         "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                         -DWORK_DIR=${VASDK_ROOT_BINARY_DIR}/consumer_${_mode}
                         -P ${VASDK_ROOT_DIR}/tests/consumer/run_consumer_test.cmake)
        set_tests_properties(vasdk_consumer_${_mode} PROPERTIES TIMEOUT 600 RESOURCE_LOCK vasdk_consumer)
    endforeach()
endif()
