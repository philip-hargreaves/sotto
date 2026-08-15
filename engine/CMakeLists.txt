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

# The amalgamation zip carries no build files; the target is declared here.
# Defines are from sqlite.org's recommended list; PRAGMAs live in Db alone
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
    PRIVATE sotto_warnings sqlite3 bcrypt crypt32)

add_library(sotto_audio STATIC
    src/adapters/audio/wasapi_capture.cpp
    src/adapters/audio/wav_source.cpp)
target_link_libraries(sotto_audio
    PUBLIC sotto_core
    PRIVATE sotto_warnings ole32 advapi32)

add_executable(sotto_engine src/main.cpp)
target_link_libraries(sotto_engine PRIVATE sotto_ipc sotto_audio sotto_storage sotto_warnings)

add_subdirectory(tests)
