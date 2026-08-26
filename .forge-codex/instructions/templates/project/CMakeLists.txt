cmake_minimum_required(VERSION 3.28)

project(
    ForgeConductorWindows
    VERSION 0.1.0
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Forge Conductor for Windows must be configured on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FORGE_ENABLE_ANALYZE "Enable MSVC static analysis" OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /EHsc)
    if(FORGE_ENABLE_ANALYZE)
        add_compile_options(/analyze)
    endif()
endif()

add_executable(forge-port-bootstrap src/Bootstrap/main.cpp)
target_compile_definitions(forge-port-bootstrap PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

enable_testing()
add_test(NAME bootstrap-runs COMMAND forge-port-bootstrap --self-test)
