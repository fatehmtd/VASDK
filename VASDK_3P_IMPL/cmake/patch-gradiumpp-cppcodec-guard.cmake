# Runs as a FetchContent PATCH_COMMAND, whose working directory is the root
# of the fetched GradiumPP source tree.

set(_file "CMakeLists.txt")
file(READ "${_file}" _contents)
if(NOT _contents MATCHES "if\\(NOT TARGET cppcodec\\)")
    string(REPLACE
        "add_library(cppcodec INTERFACE)"
        "if(NOT TARGET cppcodec)\nadd_library(cppcodec INTERFACE)\nendif()"
        _contents "${_contents}")
    file(WRITE "${_file}" "${_contents}")
endif()
