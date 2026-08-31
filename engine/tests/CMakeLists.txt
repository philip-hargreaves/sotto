include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723  # v1.17.0
    SYSTEM
)
set(BUILD_GMOCK OFF)
set(INSTALL_GTEST OFF)
# gtest defaults to the static CRT; this build is dynamic
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# googletest is not ours to keep warning-clean
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_MakeAvailable(googletest)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

add_executable(engine_tests
    core/audio_ring_test.cpp
    core/cli_args_test.cpp
    core/diar_capture_test.cpp
    core/diar_regions_test.cpp
    core/endpointer_test.cpp
    core/level_meter_test.cpp
    core/metrics_test.cpp
    core/note_label_test.cpp
    core/per_turn_test.cpp
    core/role_naming_test.cpp
    core/seg_decode_test.cpp
    core/throughput_test.cpp
    core/session_controller_test.cpp
    core/slice_refinement_test.cpp
    core/turn_assembly_test.cpp
    core/turn_reconcile_test.cpp
    core/version_test.cpp
    adapters/anchor_store_test.cpp
    adapters/capture_errors_test.cpp
    adapters/capture_timeline_test.cpp
    adapters/chunk_cipher_test.cpp
    adapters/deferred_load_test.cpp
    adapters/db_test.cpp
    adapters/fbank_test.cpp
    adapters/framing_test.cpp
    adapters/capture_devices_test.cpp
    adapters/handlers_test.cpp
    adapters/messages_test.cpp
    adapters/model_store_test.cpp
    adapters/note_prompt_test.cpp
    adapters/ov_smoke_test.cpp
    adapters/pipe_security_test.cpp
    adapters/power_throttling_test.cpp
    adapters/scripted_transcriber_test.cpp
    adapters/session_store_test.cpp
    adapters/speaker_clustering_test.cpp
    adapters/store_recovery_test.cpp
    adapters/wav_source_test.cpp
    adapters/whisper_worker_test.cpp)
target_compile_definitions(engine_tests PRIVATE
    AMBIENT_CMAKE_VERSION="${ambient_VERSION}"
    AMBIENT_FIXTURE_DIR="${ambient_SOURCE_DIR}/schema/fixtures"
    AMBIENT_CRASH_HELPER="$<TARGET_FILE:store_crash_helper>")

# The recovery test's kill victim
add_executable(store_crash_helper
    adapters/store_crash_helper.cpp)
target_link_libraries(store_crash_helper PRIVATE ambient_storage ambient_warnings)
add_dependencies(engine_tests store_crash_helper)
target_link_libraries(engine_tests PRIVATE
    ambient_ipc ambient_audio ambient_diarisation ambient_models ambient_storage ambient_transcription
    ambient_warnings openvino::genai GTest::gtest_main)

# Real-microphone tests live in their own binary under the microphone label,
# so the ci presets can exclude what a runner without audio cannot execute
add_executable(capture_tests
    adapters/wasapi_capture_test.cpp)
target_link_libraries(capture_tests PRIVATE
    ambient_audio ambient_warnings GTest::gtest_main)

# Same mechanism for tests needing the Intel GPU
add_executable(gpu_tests
    adapters/ov_device_test.cpp
    adapters/ov_runtime_test.cpp)
target_link_libraries(gpu_tests PRIVATE
    ambient_models ambient_warnings openvino::genai GTest::gtest_main)

# And for tests needing staged model weights
add_executable(models_tests
    adapters/asr_model_test.cpp
    adapters/asr_wer_test.cpp
    adapters/cluster_voiceprint_test.cpp
    adapters/diariser_pipeline_test.cpp
    adapters/embedder_model_test.cpp
    adapters/fbank_parity_test.cpp
    adapters/note_writer_test.cpp
    adapters/seg_model_test.cpp
    adapters/silero_vad_test.cpp
    adapters/speaker_embedder_test.cpp
    adapters/translate_test.cpp
    adapters/whisper_real_test.cpp
    adapters/worker_note_writer_test.cpp)
target_compile_definitions(models_tests PRIVATE
    AMBIENT_MODELS_DIR="${ambient_SOURCE_DIR}/models"
    AMBIENT_DIAR_FIXTURE_DIR="${ambient_SOURCE_DIR}/engine/tests/fixtures/diarisation")
target_link_libraries(models_tests PRIVATE
    ambient_diarisation ambient_models ambient_note ambient_transcription ambient_translate ambient_vad
    ambient_warnings nlohmann_json::nlohmann_json openvino::genai GTest::gtest_main)

# Dev evaluation tool for the research acceptance scorer; never shipped
add_executable(diar_eval_runner tools/diar_eval_runner.cpp)
target_link_libraries(diar_eval_runner PRIVATE ambient_diarisation ambient_transcription ambient_warnings)

ambient_deploy_openvino(ov_test_runtime)
add_dependencies(diar_eval_runner ov_test_runtime)
add_dependencies(engine_tests ov_test_runtime)
add_dependencies(gpu_tests ov_test_runtime)
add_dependencies(models_tests ov_test_runtime ambient_note_host)

include(GoogleTest)
# Cold CI runners can take >5 s (the default) just to start a fresh exe
gtest_discover_tests(engine_tests DISCOVERY_TIMEOUT 60)
gtest_discover_tests(capture_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS microphone)
gtest_discover_tests(gpu_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS gpu)
gtest_discover_tests(models_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS models)
