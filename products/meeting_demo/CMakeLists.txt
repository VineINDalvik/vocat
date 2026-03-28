cmake_minimum_required(VERSION 3.5)

# Reuse the ESP-VoCat BSP and managed codec components from the speaker product.
# This avoids re-downloading components that are already present.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/common_components"
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/managed_components"
)

# sdkconfig.local holds private credentials (WiFi, Volc keys) — gitignored.
# Copy sdkconfig.local.example to sdkconfig.local and fill in your values.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meeting_demo)
