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

# The schema and its migrations are SQL files, embedded so the engine stays
# a single binary
set(sotto_schema_dir ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/storage/schema)
file(READ ${sotto_schema_dir}/sotto.sql SOTTO_SCHEMA_SQL)
file(READ ${sotto_schema_dir}/migrate-2-to-3.sql SOTTO_MIGRATE_2_TO_3_SQL)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${sotto_schema_dir}/sotto.sql ${sotto_schema_dir}/migrate-2-to-3.sql)
configure_file(src/adapters/storage/schema.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/adapters/storage/schema.hpp @ONLY)

add_library(sotto_storage STATIC
    src/adapters/storage/chunk_cipher.cpp
    src/adapters/storage/db.cpp
    src/adapters/storage/sqlite_session_store.cpp)
target_include_directories(sotto_storage PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
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

add_library(sotto_note STATIC
    src/adapters/note/qwen_note_writer.cpp
    src/adapters/note/worker_note_writer.cpp)
target_link_libraries(sotto_note
    PUBLIC sotto_core
    PRIVATE sotto_models sotto_warnings nlohmann_json::nlohmann_json openvino::genai)

add_library(sotto_translate STATIC
    src/adapters/translate/nllb_translator.cpp)
target_link_libraries(sotto_translate
    PUBLIC sotto_core nlohmann_json::nlohmann_json
    PRIVATE sotto_models sotto_warnings openvino::genai)

add_library(sotto_vad STATIC
    src/adapters/vad/silero_vad.cpp)
target_link_libraries(sotto_vad
    PUBLIC sotto_core
    PRIVATE sotto_models sotto_warnings openvino::genai)

# Average-linkage clustering (fastcluster), the implementation the research
# proved identical to the sklearn reference. No CMake build upstream; the
# amalgamated source is compiled here like sqlite
FetchContent_Declare(
    hclust_cpp
    GIT_REPOSITORY https://github.com/cdalitz/hclust-cpp.git
    GIT_TAG d48fff6bba1199d80422cd37f5b635107a5a0c92
    SYSTEM
)
FetchContent_MakeAvailable(hclust_cpp)
add_library(hclust STATIC ${hclust_cpp_SOURCE_DIR}/fastcluster.cpp)
target_include_directories(hclust SYSTEM PUBLIC ${hclust_cpp_SOURCE_DIR})
# Not ours to keep warning-clean
set_target_properties(hclust PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# Feature front-end for the speaker embedder, pinned to the version the
# research validated against torchaudio. kissfft is fetched and hash-pinned
# by knf's own build
set(KALDI_NATIVE_FBANK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KALDI_NATIVE_FBANK_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(
    kaldi_native_fbank
    GIT_REPOSITORY https://github.com/csukuangfj/kaldi-native-fbank.git
    GIT_TAG b09e686fe2084732ddd30d1ef80acfc0f13eaf01  # v1.22.3
    SYSTEM
)
FetchContent_MakeAvailable(kaldi_native_fbank)
# Not ours to keep warning-clean
set_target_properties(kaldi-native-fbank-core kissfft PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_library(sotto_diarisation STATIC
    src/adapters/diarisation/anchor_store.cpp
    src/adapters/diarisation/cluster_voiceprint.cpp
    src/adapters/diarisation/diar_worker.cpp
    src/adapters/diarisation/fbank.cpp
    src/adapters/diarisation/segmenter.cpp
    src/adapters/diarisation/speaker_clustering.cpp
    src/adapters/diarisation/speaker_diariser.cpp
    src/adapters/diarisation/speaker_embedder.cpp)
target_include_directories(sotto_diarisation SYSTEM PRIVATE ${kaldi_native_fbank_SOURCE_DIR})
target_link_libraries(sotto_diarisation
    PUBLIC sotto_core sotto_models sotto_vad
    PRIVATE sotto_warnings crypt32 hclust kaldi-native-fbank-core)

add_library(sotto_audio STATIC
    src/adapters/audio/wasapi_capture.cpp
    src/adapters/audio/wasapi_player.cpp
    src/adapters/audio/wav_source.cpp)
target_link_libraries(sotto_audio
    PUBLIC sotto_core
    PRIVATE sotto_warnings ole32 advapi32)

add_executable(sotto_engine src/main.cpp)
target_link_libraries(sotto_engine PRIVATE
    sotto_ipc sotto_audio sotto_diarisation sotto_note sotto_storage sotto_transcription
    sotto_translate sotto_vad sotto_warnings)
sotto_deploy_openvino(ov_engine_runtime)
add_dependencies(sotto_engine ov_engine_runtime)

# The note model's own process; deployed beside the engine that spawns it
add_executable(sotto_note_host src/note_host_main.cpp)
target_link_libraries(sotto_note_host PRIVATE
    sotto_ipc sotto_models sotto_note sotto_warnings)
add_dependencies(sotto_note_host ov_engine_runtime)
add_dependencies(sotto_engine sotto_note_host)

add_subdirectory(tests)
