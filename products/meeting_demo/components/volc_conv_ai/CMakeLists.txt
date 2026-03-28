# volc_conv_ai — thin wrapper that adapts the SDK subdirectory as an IDF component.
# The SDK lives in ../volc_conv_ai_sdk/volc_conv_ai/ (git submodule).

set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../volc_conv_ai_sdk/volc_conv_ai")

include("${SDK_DIR}/cmake/common.cmake")
include("${SDK_DIR}/cmake/espressif.cmake")
