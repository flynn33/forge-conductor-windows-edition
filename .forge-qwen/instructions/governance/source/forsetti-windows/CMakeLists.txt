# Forsetti Framework for Windows
# Copyright (c) 2026 James Daley. All Rights Reserved.
# Proprietary and Confidential.

cmake_minimum_required(VERSION 3.28)
project(ForsettiFramework
    VERSION 0.2.0
    LANGUAGES CXX
    DESCRIPTION "Forsetti Framework for Windows — Modular Runtime Framework"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FORSETTI_BUILD_HOST_TEMPLATE "Build the ForsettiHostTemplate target." ON)
option(FORSETTI_BUILD_SAMPLES "Build downstream Forsetti samples." ON)

# MSVC warnings as errors
if(MSVC)
    add_compile_options(/W4 /WX)
endif()

# Find nlohmann/json via vcpkg
find_package(nlohmann_json CONFIG REQUIRED)

# Library targets
add_subdirectory(src/ForsettiCore)
add_subdirectory(src/ForsettiPlatform)
if(FORSETTI_BUILD_HOST_TEMPLATE)
    add_subdirectory(src/ForsettiHostTemplate)
endif()
add_subdirectory(src/ForsettiExampleServiceModule)
add_subdirectory(src/ForsettiExampleUIModule)
add_subdirectory(src/ForsettiExampleAppModule)

if(FORSETTI_BUILD_SAMPLES AND FORSETTI_BUILD_HOST_TEMPLATE)
    add_subdirectory(samples/ForsettiDemo)
endif()

# Tests
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
