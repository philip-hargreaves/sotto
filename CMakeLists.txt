cmake_minimum_required(VERSION 3.25)

project(ambient
    VERSION 0.1.0
    DESCRIPTION "Ambient clinical AI, entirely on-device"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Applies to every target below; the tests exempt googletest around its fetch
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

add_library(ambient_warnings INTERFACE)
target_compile_options(ambient_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>)

add_subdirectory(engine)

# Developer weight acquisition: verified download into the repo model store
add_custom_target(fetch-models
    COMMAND dotnet run --project "${ambient_SOURCE_DIR}/app/Ambient.Fetch" -c Release --
            fetch "${ambient_SOURCE_DIR}/weights" "${ambient_SOURCE_DIR}"
    VERBATIM)
