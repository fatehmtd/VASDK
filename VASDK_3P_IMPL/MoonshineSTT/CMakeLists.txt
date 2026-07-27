add_library(vasdk_moonshine_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    MoonshineSttProvider.hpp
    MoonshineSttProvider.cpp
    ${moonshine_bintokenizer_SOURCE_DIR}/core/bin-tokenizer/bin-tokenizer.h
    ${moonshine_bintokenizer_SOURCE_DIR}/core/bin-tokenizer/bin-tokenizer.cpp
    ${moonshine_bintokenizer_SOURCE_DIR}/core/moonshine-utils/string-utils.h
    ${moonshine_bintokenizer_SOURCE_DIR}/core/moonshine-utils/string-utils.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_moonshine_stt
    BASE_NAME VASDK_MOONSHINE_STT
    EXPORT_FILE_NAME vasdk_moonshine_stt_export.h
)

target_include_directories(vasdk_moonshine_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(vasdk_moonshine_stt PRIVATE
    ${moonshine_bintokenizer_SOURCE_DIR}/core/bin-tokenizer
    ${moonshine_bintokenizer_SOURCE_DIR}/core/moonshine-utils
    ${onnxruntime_SOURCE_DIR}/include
)

target_link_libraries(vasdk_moonshine_stt
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_moonshine_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES MoonshineSttProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MoonshineSTT
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_moonshine_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MoonshineSTT
)
