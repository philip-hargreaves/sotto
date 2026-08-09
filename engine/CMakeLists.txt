# core has no deps; OpenVINO and Win32 live in adapters
add_library(sotto_core INTERFACE)
target_include_directories(sotto_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
