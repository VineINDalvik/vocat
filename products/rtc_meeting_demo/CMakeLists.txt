cmake_minimum_required(VERSION 3.5)

# Reuse speaker's BSP and managed components
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/common_components"
    "${CMAKE_CURRENT_LIST_DIR}/../speaker/managed_components"
)

# sdkconfig.local holds private credentials (gitignored)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rtc_meeting_demo)
