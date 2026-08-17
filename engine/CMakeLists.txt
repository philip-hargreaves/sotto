# core has no deps; OpenVINO and Win32 live in adapters
add_library(sotto_core INTERFACE)
target_include_directories(sotto_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Toolchain is fetched and pinned by tools/get-openvino.ps1
if(NOT EXISTS "${sotto_SOURCE_DIR}/external/openvino")
    message(FATAL_ERROR "OpenVINO not found: run tools/get-openvino.ps1 first")
endif()
find_package(OpenVINOGenAI REQUIRED)

# Archive ships Debug and Release runtimes. Shipping trims this to the minimal set
set(_ov_root "${sotto_SOURCE_DIR}/external/openvino/runtime")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(GLOB _ov_dlls "${_ov_root}/bin/intel64/Debug/*.dll"
         "${_ov_root}/bin/intel64/Debug/cache.json"
         "${_ov_root}/3rdparty/tbb/bin/*_debug.dll")
else()
    file(GLOB _ov_dlls "${_ov_root}/bin/intel64/Release/*.dll"
         "${_ov_root}/bin/intel64/Release/cache.json")
    file(GLOB _tbb_dlls "${_ov_root}/3rdparty/tbb/bin/*.dll")
    list(FILTER _tbb_dlls EXCLUDE REGEX "_debug")
    list(APPEND _ov_dlls ${_tbb_dlls})
endif()
# One deployment per output directory; targets there depend on the stamp
function(sotto_deploy_openvino stamp_target)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ov.deployed"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ov_dlls}
                "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/ov.deployed"
        VERBATIM)
    add_custom_target(${stamp_target} DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ov.deployed")
endfunction()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b  # v3.12.0
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(sotto_ipc STATIC
    src/adapters/ipc/handlers.cpp
    src/adapters/ipc/pipe_security.cpp
    src/adapters/ipc/pipe_server.cpp)
target_link_libraries(sotto_ipc
    PUBLIC sotto_core sotto_models nlohmann_json::nlohmann_json
    PRIVATE sotto_warnings advapi32)

FetchContent_Declare(
    sqlite3
    URL https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

enable_language(C)
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_LOAD_EXTENSION)
# Not ours to keep warning-clean
set_target_properties(sqlite3 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_library(sotto_storage STATIC
    src/adapters/storage/chunk_cipher.cpp
    src/adapters/storage/db.cpp
    src/adapters/storage/sqlite_session_store.cpp)
target_link_libraries(sotto_storage
    PUBLIC sotto_core
    PRIVATE sotto_warnings sqlite3 bcrypt crypt32 nlohmann_json::nlohmann_json)

add_library(sotto_models STATIC
    src/adapters/models/model_store.cpp
    src/adapters/models/ov_runtime.cpp)
target_link_libraries(sotto_models
    PUBLIC sotto_core nlohmann_json::nlohmann_json openvino::genai
    PRIVATE sotto_warnings bcrypt)

add_library(sotto_transcription STATIC
    src/adapters/transcription/whisper_transcriber.cpp)
target_link_libraries(sotto_transcription
    PUBLIC sotto_core
    PRIVATE sotto_models sotto_warnings openvino::genai)

add_library(sotto_audio STATIC
    src/adapters/audio/wasapi_capture.cpp
    src/adapters/audio/wav_source.cpp)
target_link_libraries(sotto_audio
    PUBLIC sotto_core
    PRIVATE sotto_warnings ole32 advapi32)

add_executable(sotto_engine src/main.cpp)
target_link_libraries(sotto_engine PRIVATE
    sotto_ipc sotto_audio sotto_storage sotto_transcription sotto_warnings)
sotto_deploy_openvino(ov_engine_runtime)
add_dependencies(sotto_engine ov_engine_runtime)

add_subdirectory(tests)
