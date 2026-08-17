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
    core/level_meter_test.cpp
    core/session_controller_test.cpp
    core/version_test.cpp
    core/window_cutter_test.cpp
    adapters/capture_errors_test.cpp
    adapters/capture_timeline_test.cpp
    adapters/chunk_cipher_test.cpp
    adapters/db_test.cpp
    adapters/framing_test.cpp
    adapters/handlers_test.cpp
    adapters/messages_test.cpp
    adapters/model_store_test.cpp
    adapters/ov_smoke_test.cpp
    adapters/pipe_security_test.cpp
    adapters/scripted_transcriber_test.cpp
    adapters/session_store_test.cpp
    adapters/store_recovery_test.cpp
    adapters/wav_source_test.cpp)
target_compile_definitions(engine_tests PRIVATE
    SOTTO_CMAKE_VERSION="${sotto_VERSION}"
    SOTTO_FIXTURE_DIR="${sotto_SOURCE_DIR}/schema/fixtures"
    SOTTO_CRASH_HELPER="$<TARGET_FILE:store_crash_helper>")

# The recovery test's kill victim
add_executable(store_crash_helper
    adapters/store_crash_helper.cpp)
target_link_libraries(store_crash_helper PRIVATE sotto_storage sotto_warnings)
add_dependencies(engine_tests store_crash_helper)
target_link_libraries(engine_tests PRIVATE
    sotto_ipc sotto_audio sotto_models sotto_storage sotto_warnings openvino::genai
    GTest::gtest_main)

# Real-microphone tests live in their own binary under the microphone label,
# so the ci presets can exclude what a runner without audio cannot execute
add_executable(capture_tests
    adapters/wasapi_capture_test.cpp)
target_link_libraries(capture_tests PRIVATE
    sotto_audio sotto_warnings GTest::gtest_main)

# Same mechanism for tests needing the Intel GPU
add_executable(gpu_tests
    adapters/ov_device_test.cpp
    adapters/ov_runtime_test.cpp)
target_link_libraries(gpu_tests PRIVATE
    sotto_models sotto_warnings openvino::genai GTest::gtest_main)

# And for tests needing staged model weights
add_executable(models_tests
    adapters/asr_model_test.cpp)
target_compile_definitions(models_tests PRIVATE
    SOTTO_MODELS_DIR="${sotto_SOURCE_DIR}/models")
target_link_libraries(models_tests PRIVATE
    sotto_models sotto_warnings openvino::genai GTest::gtest_main)

sotto_deploy_openvino(ov_test_runtime)
add_dependencies(engine_tests ov_test_runtime)
add_dependencies(gpu_tests ov_test_runtime)
add_dependencies(models_tests ov_test_runtime)

include(GoogleTest)
gtest_discover_tests(engine_tests)
gtest_discover_tests(capture_tests PROPERTIES LABELS microphone)
gtest_discover_tests(gpu_tests PROPERTIES LABELS gpu)
gtest_discover_tests(models_tests PROPERTIES LABELS models)
