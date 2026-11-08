# ctest vasdk_command: the vasdk command against a manifest of its own (a local file:// source), so
# it runs offline: pull, list, verify, remove, and the license gate.
#   cmake -DVASDK_COMMAND=<vasdk> -DWORK_DIR=<dir> -P test_vasdk_command.cmake

file(REMOVE_RECURSE ${WORK_DIR})
file(WRITE ${WORK_DIR}/sources/abc.txt "abc")
set(_abc_sha256 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad)
set(_source "file://${WORK_DIR}/sources/abc.txt")
if(NOT _source MATCHES "^file:///")
    set(_source "file:///${WORK_DIR}/sources/abc.txt")   # file:///C:/...
endif()
file(WRITE ${WORK_DIR}/manifest.json "{\"schema\": 1, \"models\": [
  {\"id\": \"small\", \"providers\": [\"vad/test\"], \"license\": \"MIT\",
   \"files\": [{\"path\": \"small/abc.txt\", \"url\": \"${_source}\", \"sha256\": \"${_abc_sha256}\", \"size\": 3}]},
  {\"id\": \"terms\", \"providers\": [], \"license\": \"CC-BY-NC-4.0\", \"requires_acceptance\": true,
   \"files\": [{\"path\": \"terms.txt\", \"url\": \"${_source}\", \"sha256\": \"${_abc_sha256}\", \"size\": 3}]}]}\n")
file(WRITE ${WORK_DIR}/engine.txtpb "providers { vad { id: \"test\" } }\n")
set(_store ${WORK_DIR}/store)

function(vasdk_command EXPECT)
    execute_process(COMMAND ${VASDK_COMMAND} ${ARGN} --dir ${_store} --manifest ${WORK_DIR}/manifest.json
                    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _output)
    message(STATUS "vasdk ${ARGN}: ${_result}\n${_output}")
    if(NOT _result EQUAL EXPECT)
        message(FATAL_ERROR "vasdk ${ARGN} exited ${_result}, expected ${EXPECT}")
    endif()
    set(_output "${_output}" PARENT_SCOPE)
endfunction()

vasdk_command(0 version)
vasdk_command(0 models pull --config ${WORK_DIR}/engine.txtpb)
if(NOT EXISTS ${_store}/small/abc.txt)
    message(FATAL_ERROR "pull --config didn't download small/abc.txt")
endif()
vasdk_command(0 models list)
if(NOT _output MATCHES "small +present")
    message(FATAL_ERROR "list doesn't show small as present")
endif()
vasdk_command(0 models verify small)
vasdk_command(1 models pull terms)                  # its license needs accepting
vasdk_command(0 models pull terms --accept-license)
vasdk_command(0 models remove small)
if(EXISTS ${_store}/small/abc.txt)
    message(FATAL_ERROR "remove left small/abc.txt")
endif()
vasdk_command(1 models pull small --offline)        # missing, and offline
vasdk_command(2 models)                             # usage
file(REMOVE_RECURSE ${WORK_DIR})
