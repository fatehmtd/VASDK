add_library(vasdk_energy_zcr_vad ${VASDK_THIRDPARTY_LIB_TYPE}
    EnergyZCRVAD.h EnergyZCRVAD.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_energy_zcr_vad
    BASE_NAME VASDK_ENERGY_ZCR_VAD
    EXPORT_FILE_NAME vasdk_energy_zcr_vad_export.h
)

target_include_directories(vasdk_energy_zcr_vad PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_energy_zcr_vad
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_energy_zcr_vad
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES EnergyZCRVAD.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/EnergyZCRVAD
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_energy_zcr_vad_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/EnergyZCRVAD
)
