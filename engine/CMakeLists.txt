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

# A library, not sources on the executable, so the tests can link the adapters.
# Static rather than object: object files do not survive an intermediate target,
# and nothing here self-registers, so the unreferenced-object rule cannot bite.
add_library(sotto_ipc STATIC
    src/adapters/ipc/handlers.cpp
    src/adapters/ipc/pipe_security.cpp
    src/adapters/ipc/pipe_server.cpp)
target_link_libraries(sotto_ipc
    PUBLIC sotto_core nlohmann_json::nlohmann_json
    PRIVATE sotto_warnings advapi32)

add_library(sotto_audio STATIC
    src/adapters/audio/wasapi_capture.cpp
    src/adapters/audio/wav_source.cpp)
target_link_libraries(sotto_audio
    PUBLIC sotto_core
    PRIVATE sotto_warnings ole32 advapi32)

add_executable(sotto_engine src/main.cpp)
target_link_libraries(sotto_engine PRIVATE sotto_ipc sotto_audio sotto_warnings)

add_subdirectory(tests)
