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
FetchContent_MakeAvailable(googletest)

add_executable(core_tests core/version_test.cpp)
target_compile_definitions(core_tests PRIVATE SOTTO_CMAKE_VERSION="${sotto_VERSION}")
target_link_libraries(core_tests PRIVATE sotto_core sotto_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(core_tests)
