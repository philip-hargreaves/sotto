cmake_minimum_required(VERSION 3.25)

project(sotto
    VERSION 0.1.0
    DESCRIPTION "Ambient clinical AI, entirely on-device"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(sotto_warnings INTERFACE)
target_compile_options(sotto_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /WX>)

add_subdirectory(engine)
add_subdirectory(tests)
