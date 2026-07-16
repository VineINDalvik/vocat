cmake_minimum_required(VERSION 3.5)

# Reuse speaker's BSP. Reuse its generated managed components only when they
# exist; a clean checkout resolves this project's declared dependencies itself.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/common_components/esp_vocat"
)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../speaker/managed_components")
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../speaker/managed_components")
endif()

# sdkconfig.local holds private credentials (gitignored)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
# Copy sdkconfig.local.example to sdkconfig.local and fill in your credentials.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ws_meeting_demo)
