# vasdk_model_downloads: VASDKModels.cmake, offline. cmake -DWORK=<dir> -P test_model_downloads.cmake
#
# 1. Against a manifest of file:// sources: per-CPU files, STRIP, a file kept when it matches, one
#    replaced when it doesn't, a wrong SHA-256 failing without leaving a file, RESULT.
# 2. Every model id the Download*.cmake scripts can ask for is in the real manifest.

if(NOT WORK)
    message(FATAL_ERROR "pass -DWORK=<scratch directory>")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/source")

function(fail message)
    message(FATAL_ERROR "FAILED: ${message}")
endfunction()
function(expect_true value message)
    if(NOT value)
        fail("${message}")
    endif()
endfunction()
function(expect_false value message)
    if(value)
        fail("${message}")
    endif()
endfunction()
function(expect_content file expected message)
    if(NOT EXISTS "${file}")
        fail("${message} (no ${file})")
    endif()
    file(READ "${file}" content)
    if(NOT content STREQUAL expected)
        fail("${message} (${file} holds '${content}')")
    endif()
endfunction()
function(expect_missing file message)
    if(EXISTS "${file}")
        fail("${message} (${file} exists)")
    endif()
endfunction()

# Sources and their hashes
file(WRITE "${WORK}/source/common.txt" "common to every CPU")
file(WRITE "${WORK}/source/arm.bin" "arm64 weights")
file(WRITE "${WORK}/source/x86.bin" "x86_64 weights")
foreach(name common arm x86)
    if(name STREQUAL "common")
        set(file_name "common.txt")
    else()
        set(file_name "${name}.bin")
    endif()
    file(SHA256 "${WORK}/source/${file_name}" hash_${name})
    file(SIZE "${WORK}/source/${file_name}" size_${name})
endforeach()
if(WORK MATCHES "^/")
    set(source "file://${WORK}/source")
else()
    set(source "file:///${WORK}/source")
endif()

file(WRITE "${WORK}/manifest.json" "{\"schema\": 1, \"models\": [
  {\"id\": \"demo\", \"license\": \"MIT\", \"license_url\": \"https://example.com\", \"files\": [
    {\"path\": \"demo/common.txt\", \"url\": \"${source}/common.txt\", \"sha256\": \"${hash_common}\", \"size\": ${size_common}},
    {\"path\": \"demo/model.bin\", \"arch\": \"arm64\", \"url\": \"${source}/arm.bin\", \"sha256\": \"${hash_arm}\", \"size\": ${size_arm}},
    {\"path\": \"demo/model.bin\", \"arch\": \"x86_64\", \"url\": \"${source}/x86.bin\", \"sha256\": \"${hash_x86}\", \"size\": ${size_x86}}]},
  {\"id\": \"broken\", \"license\": \"MIT\", \"license_url\": \"https://example.com\", \"files\": [
    {\"path\": \"broken.txt\", \"url\": \"${source}/common.txt\", \"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\", \"size\": ${size_common}}]}
]}")

include("${CMAKE_CURRENT_LIST_DIR}/VASDKModels.cmake")
set(VASDK_MODEL_MANIFEST "${WORK}/manifest.json")

# 1a. x86_64 (AMD64, as Windows names it): the shared file and the x86_64 one, prefix taken off
set(CMAKE_SYSTEM_PROCESSOR AMD64)
vasdk_download_model(demo DIRECTORY "${WORK}/x86" STRIP demo/ RESULT ok)
expect_true("${ok}" "demo on x86_64 reports success")
expect_content("${WORK}/x86/model.bin" "x86_64 weights" "x86_64 gets the x86_64 file")
expect_content("${WORK}/x86/common.txt" "common to every CPU" "STRIP puts demo/common.txt at <dir>/common.txt")
expect_missing("${WORK}/x86/demo" "STRIP leaves no demo/ folder")

# 1b. arm64 (aarch64 is the same CPU), without STRIP
set(CMAKE_SYSTEM_PROCESSOR aarch64)
vasdk_download_model(demo DIRECTORY "${WORK}/arm" RESULT ok)
expect_content("${WORK}/arm/demo/model.bin" "arm64 weights" "aarch64 gets the arm64 file, under demo/ without STRIP")

# 1c. A matching file stays; a changed one is replaced
file(TIMESTAMP "${WORK}/arm/demo/common.txt" before "%s")
file(WRITE "${WORK}/arm/demo/model.bin" "tampered")   # another size
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
vasdk_download_model(demo DIRECTORY "${WORK}/arm" RESULT ok)
expect_true("${ok}" "the second pull succeeds")
file(TIMESTAMP "${WORK}/arm/demo/common.txt" after "%s")
if(NOT before STREQUAL after)
    fail("a file that matches the manifest is not downloaded again")
endif()
expect_content("${WORK}/arm/demo/model.bin" "arm64 weights" "a file of another size is replaced")
file(WRITE "${WORK}/arm/demo/model.bin" "arm64 WEIGHTS")   # same size, other bytes: caught by the hash
vasdk_download_model(demo DIRECTORY "${WORK}/arm")
expect_content("${WORK}/arm/demo/model.bin" "arm64 weights" "a same-size file with other bytes is replaced")

# 1d. A wrong SHA-256: a warning, RESULT off, no file and no .part left
vasdk_download_model(broken DIRECTORY "${WORK}/broken" RESULT ok)
expect_false("${ok}" "a file whose SHA-256 doesn't match reports failure")
expect_missing("${WORK}/broken/broken.txt" "a file whose SHA-256 doesn't match is not kept")
expect_missing("${WORK}/broken/broken.txt.part" "no .part is left behind")

# 2. Every id the build asks for is in the real manifest
file(READ "${CMAKE_CURRENT_LIST_DIR}/../models/manifest.json" manifest)
string(JSON count LENGTH "${manifest}" models)
math(EXPR last "${count} - 1")
set(identifiers "")
foreach(index RANGE ${last})
    string(JSON identifier GET "${manifest}" models ${index} id)
    list(APPEND identifiers "${identifier}")
endforeach()
# The variants the scripts' switches pick, and the literal ids in the scripts
set(wanted gliner gliner_multi moonshine moonshine_tiny moonshine_medium phonon_tts phonon_tts_en
           lfm2.5-1.2b-instruct whispercpp)
foreach(size tiny.en tiny base.en base small.en small medium.en large-v3)
    list(APPEND wanted whispercpp_${size})
endforeach()
file(GLOB scripts "${CMAKE_CURRENT_LIST_DIR}/Download*.cmake")
foreach(script IN LISTS scripts)
    file(STRINGS "${script}" calls REGEX "vasdk_download_model[(][a-z0-9_.-]+ ")
    foreach(call IN LISTS calls)
        string(REGEX MATCH "vasdk_download_model[(]([a-z0-9_.-]+) " matched "${call}")
        list(APPEND wanted "${CMAKE_MATCH_1}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES wanted)
foreach(identifier IN LISTS wanted)
    list(FIND identifiers "${identifier}" found)
    if(found EQUAL -1)
        fail("model '${identifier}', which the build can download, is not in manifest.json")
    endif()
endforeach()
list(LENGTH wanted wanted_count)
message(STATUS "vasdk_model_downloads: passed (${wanted_count} model ids the build uses are in the manifest)")
