# Where VASDK's libraries come from (docs/building.md, "Dependencies"):
#   FETCH   download the pinned version and build it (the default, and what a source build has always done)
#   SYSTEM  find_package only, never download: Conan, Homebrew and distro packages
#   AUTO    find_package, else download
# A target the outer project already defines (spdlog::spdlog, ...) is used as is in every mode.
set(VASDK_DEPENDENCY_SOURCE "FETCH" CACHE STRING "Where VASDK's dependencies come from: FETCH, SYSTEM or AUTO")
set_property(CACHE VASDK_DEPENDENCY_SOURCE PROPERTY STRINGS FETCH SYSTEM AUTO)
if(NOT VASDK_DEPENDENCY_SOURCE MATCHES "^(FETCH|SYSTEM|AUTO)$")
    message(FATAL_ERROR "VASDK_DEPENDENCY_SOURCE is '${VASDK_DEPENDENCY_SOURCE}': FETCH, SYSTEM or AUTO")
endif()

# A fetched dependency's own install rules stay out of VASDK's install: FetchContent's
# EXCLUDE_FROM_ALL (CMake 3.28+). With an older CMake, a plain install also holds what they install.
set(VASDK_FETCH_EXCLUDE "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(VASDK_FETCH_EXCLUDE EXCLUDE_FROM_ALL)
endif()
# The same, for those that have their own switch
set(HTTPLIB_INSTALL OFF CACHE BOOL "cpp-httplib: install its files (VASDK: no)")
set(INSTALL_PFFFT OFF CACHE BOOL "pffft: install its files (VASDK: no)")

# vasdk_dependency(<package> TARGET <target> [FIND <find_package arguments>...] DECLARE <FetchContent_Declare arguments>...)
#
# Provides <target>: already defined, else find_package(<package> <FIND...>) under SYSTEM and AUTO,
# else FetchContent with <DECLARE...> under FETCH and AUTO. A macro, not a function, so the
# variables FetchContent sets (<package>_SOURCE_DIR, ...) reach the caller.
macro(vasdk_dependency _vasdk_dep_package)
    cmake_parse_arguments(_vasdk_dep "" "TARGET" "FIND;DECLARE" ${ARGN})
    if(NOT TARGET ${_vasdk_dep_TARGET})
        if(NOT VASDK_DEPENDENCY_SOURCE STREQUAL "FETCH")
            find_package(${_vasdk_dep_package} ${_vasdk_dep_FIND} QUIET)
            if(TARGET ${_vasdk_dep_TARGET})
                # Imported targets are directory-scoped: the providers link it from other directories.
                get_target_property(_vasdk_dep_aliased ${_vasdk_dep_TARGET} ALIASED_TARGET)
                if(NOT _vasdk_dep_aliased)
                    set_target_properties(${_vasdk_dep_TARGET} PROPERTIES IMPORTED_GLOBAL TRUE)
                endif()
                message(STATUS "VASDK: ${_vasdk_dep_package} from the system (${_vasdk_dep_TARGET})")
            elseif(VASDK_DEPENDENCY_SOURCE STREQUAL "SYSTEM")
                message(FATAL_ERROR "VASDK: ${_vasdk_dep_package} not found and VASDK_DEPENDENCY_SOURCE is SYSTEM: "
                                    "install it (Conan, your package manager) or use FETCH or AUTO")
            endif()
        endif()
        if(NOT TARGET ${_vasdk_dep_TARGET})
            fetchcontent_declare(${_vasdk_dep_package} ${_vasdk_dep_DECLARE} ${VASDK_FETCH_EXCLUDE})
            fetchcontent_makeavailable(${_vasdk_dep_package})
        endif()
    endif()
endmacro()
