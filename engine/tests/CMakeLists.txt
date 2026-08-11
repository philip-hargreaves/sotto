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
    core/version_test.cpp
    adapters/framing_test.cpp
    adapters/messages_test.cpp)
target_compile_definitions(engine_tests PRIVATE
    SOTTO_CMAKE_VERSION="${sotto_VERSION}"
    SOTTO_FIXTURE_DIR="${sotto_SOURCE_DIR}/schema/fixtures")
target_link_libraries(engine_tests PRIVATE
    sotto_core sotto_warnings nlohmann_json::nlohmann_json GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(engine_tests)
