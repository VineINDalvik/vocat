# esp_vocat uses the speaker product's patched LVGL port. Keep this demo on
# the same component implementation so a clean checkout builds reproducibly.
set(WS_MEETING_LVGL_COMPONENT
    "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/espressif__esp_lvgl_port")
set(WS_MEETING_LVGL_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/../speaker/tools/patches/esp_lvgl_port.patch")

if(NOT EXISTS "${WS_MEETING_LVGL_COMPONENT}")
    message(FATAL_ERROR "esp_lvgl_port managed component not found")
endif()
if(NOT EXISTS "${WS_MEETING_LVGL_PATCH}")
    message(FATAL_ERROR "esp_lvgl_port patch not found")
endif()

execute_process(
    COMMAND patch --dry-run --forward --strip=1
            --input=${WS_MEETING_LVGL_PATCH}
    WORKING_DIRECTORY "${WS_MEETING_LVGL_COMPONENT}"
    RESULT_VARIABLE WS_MEETING_PATCH_CHECK
    ERROR_QUIET
)
if(WS_MEETING_PATCH_CHECK EQUAL 0)
    execute_process(
        COMMAND patch --forward --strip=1
                --input=${WS_MEETING_LVGL_PATCH}
        WORKING_DIRECTORY "${WS_MEETING_LVGL_COMPONENT}"
        RESULT_VARIABLE WS_MEETING_PATCH_RESULT
        ERROR_VARIABLE WS_MEETING_PATCH_ERROR
    )
    if(NOT WS_MEETING_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to patch esp_lvgl_port: ${WS_MEETING_PATCH_ERROR}")
    endif()
else()
    execute_process(
        COMMAND patch --dry-run --reverse --strip=1
                --input=${WS_MEETING_LVGL_PATCH}
        WORKING_DIRECTORY "${WS_MEETING_LVGL_COMPONENT}"
        RESULT_VARIABLE WS_MEETING_PATCHED_CHECK
        ERROR_QUIET
    )
    if(NOT WS_MEETING_PATCHED_CHECK EQUAL 0)
        message(FATAL_ERROR "esp_lvgl_port is neither clean nor correctly patched")
    endif()
endif()
