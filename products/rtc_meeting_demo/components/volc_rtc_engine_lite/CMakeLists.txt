# VolcEngineRTCLite SDK - ESP-IDF component wrapper
# Wraps the prebuilt static library for ESP32-S3

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES mbedtls espressif__zlib json lwip
)

# Link prebuilt static library for ESP32-S3
if(CONFIG_IDF_TARGET STREQUAL "esp32s3")
    add_prebuilt_library(volc_engine_rtc_lite
        "${CMAKE_CURRENT_LIST_DIR}/libs/esp32s3/libVolcEngineRTCLite.a"
        REQUIRES mbedtls espressif__zlib json lwip
    )
    target_link_libraries(${COMPONENT_LIB} INTERFACE volc_engine_rtc_lite)
else()
    message(FATAL_ERROR "VolcEngineRTCLite: unsupported target ${CONFIG_IDF_TARGET}")
endif()
