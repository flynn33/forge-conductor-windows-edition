cmake_policy(SET CMP0091 NEW)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "Forsetti C++ language standard" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "Forsetti requires C++20" FORCE)
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "Disable compiler language extensions" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "Use the MSVC DLL runtime for every configuration" FORCE)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configurations" FORCE)

if(NOT CMAKE_SYSTEM_VERSION STREQUAL "10.0.26100.0")
    message(FATAL_ERROR "Forsetti external build requires Windows SDK 10.0.26100.0.")
endif()
if(VCPKG_MANIFEST_MODE)
    message(FATAL_ERROR "UP-014 requires VCPKG_MANIFEST_MODE=OFF for the Forsetti external build.")
endif()
