# core has no deps; OpenVINO and Win32 live in adapters
add_library(sotto_core INTERFACE)
target_include_directories(sotto_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b  # v3.12.0
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(sotto_engine
    src/main.cpp
    src/adapters/ipc/pipe_security.cpp
    src/adapters/ipc/pipe_server.cpp)
target_link_libraries(sotto_engine PRIVATE
    sotto_core sotto_warnings nlohmann_json::nlohmann_json advapi32)

add_subdirectory(tests)
