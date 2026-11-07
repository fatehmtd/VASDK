#  SentencePiece 
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/FetchSentencepiece.cmake)

#  Library 

add_library(vasdk_pocket_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    PocketVoiceSynthesisProvider.hpp
    PocketVoiceSynthesisProvider.cpp
    impl/PocketTTSImpl.hpp
    impl/PocketTTSImpl.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_pocket_tts
    BASE_NAME VASDK_POCKET_TTS
    EXPORT_FILE_NAME vasdk_pocket_tts_export.h
)

target_include_directories(vasdk_pocket_tts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/impl
        ${sentencepiece_SOURCE_DIR}/src
)

# Compile-time default for LSD (ODE) steps:
#   Android → 4 steps (fast, battery-friendly)
#   Desktop → 10 steps (matches JS reference MAX_LSD=10 for best quality)
if(ANDROID)
    target_compile_definitions(vasdk_pocket_tts
        PRIVATE POCKET_TTS_DEFAULT_LSD_STEPS=4)
else()
    target_compile_definitions(vasdk_pocket_tts
        PRIVATE POCKET_TTS_DEFAULT_LSD_STEPS=10)
endif()

target_link_libraries(vasdk_pocket_tts
    PUBLIC  onnxruntime_lib
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
    PRIVATE sentencepiece-static  # target name from google/sentencepiece CMake
)

#  Install 

install(TARGETS vasdk_pocket_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES PocketVoiceSynthesisProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PocketTTS
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_pocket_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PocketTTS
)
