include_guard(GLOBAL)

include(ExternalProject)

set(FORGE_FORSETTI_VERSION "0.2.0")
set(FORGE_FORSETTI_ARCHIVE_SHA256 "3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d")
set(FORGE_FORSETTI_SOURCE_TREE_SHA256 "cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28")
set(FORGE_FORSETTI_SOURCE_DIR "${PROJECT_SOURCE_DIR}/.forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main")
set(FORGE_FORSETTI_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/forsetti-framework")

if(NOT IS_DIRECTORY "${FORGE_FORSETTI_SOURCE_DIR}/include")
    message(FATAL_ERROR "The pinned Forsetti 0.2.0 source root is missing: ${FORGE_FORSETTI_SOURCE_DIR}")
endif()

file(SHA256 "${FORGE_FORSETTI_SOURCE_DIR}/CMakeLists.txt" _forsetti_cmake_sha256)
if(NOT _forsetti_cmake_sha256 STREQUAL "d586bb4b7c9f6d00a987357c0df916718485aba34c13c86a3b5050c32cdbaa3d")
    message(FATAL_ERROR "The sealed Forsetti top-level CMakeLists.txt has changed.")
endif()

file(SHA256 "${FORGE_FORSETTI_SOURCE_DIR}/vcpkg.json" _forsetti_vcpkg_sha256)
if(NOT _forsetti_vcpkg_sha256 STREQUAL "7aa9072e3c1c9cb37512e17e95cbfb6367eec07b40165d44d9042b3442806990")
    message(FATAL_ERROR "The sealed Forsetti vcpkg manifest has changed.")
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "A valid vcpkg CMAKE_TOOLCHAIN_FILE is required.")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
    message(FATAL_ERROR "VCPKG_TARGET_TRIPLET is required.")
endif()

set(_forsetti_cmake_args
    "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
    "-DCMAKE_SYSTEM_VERSION:STRING=10.0.26100.0"
    "-DCMAKE_PROJECT_INCLUDE_BEFORE:FILEPATH=${PROJECT_SOURCE_DIR}/cmake/ForsettiExternalPolicy.cmake"
    "-DVCPKG_TARGET_TRIPLET:STRING=${VCPKG_TARGET_TRIPLET}"
    "-DVCPKG_MANIFEST_MODE:BOOL=OFF"
    "-DFORSETTI_BUILD_HOST_TEMPLATE:BOOL=ON"
    "-DFORSETTI_BUILD_SAMPLES:BOOL=OFF"
    "-DBUILD_TESTING:BOOL=OFF"
)
if(DEFINED VCPKG_HOST_TRIPLET AND NOT VCPKG_HOST_TRIPLET STREQUAL "")
    list(APPEND _forsetti_cmake_args "-DVCPKG_HOST_TRIPLET:STRING=${VCPKG_HOST_TRIPLET}")
endif()

ExternalProject_Add(forsetti_external
    SOURCE_DIR "${FORGE_FORSETTI_SOURCE_DIR}"
    BINARY_DIR "${FORGE_FORSETTI_BINARY_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS ${_forsetti_cmake_args}
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG>
        --target ForsettiHostTemplate --parallel
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS
        "${FORGE_FORSETTI_BINARY_DIR}/src/ForsettiCore/Debug/ForsettiCore.lib"
        "${FORGE_FORSETTI_BINARY_DIR}/src/ForsettiCore/Release/ForsettiCore.lib"
        "${FORGE_FORSETTI_BINARY_DIR}/src/ForsettiPlatform/Debug/ForsettiPlatform.lib"
        "${FORGE_FORSETTI_BINARY_DIR}/src/ForsettiPlatform/Release/ForsettiPlatform.lib"
        "${FORGE_FORSETTI_BINARY_DIR}/src/ForsettiHostTemplate/Debug/ForsettiHostTemplate.lib"
        "${FORGE_FORSETTI_BINARY_DIR}/src/ForsettiHostTemplate/Release/ForsettiHostTemplate.lib"
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
)

function(_forge_import_forsetti_library target_name output_name component_dir)
    add_library(${target_name} STATIC IMPORTED GLOBAL)
    set_target_properties(${target_name} PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release"
        IMPORTED_LOCATION_DEBUG
            "${FORGE_FORSETTI_BINARY_DIR}/src/${component_dir}/Debug/${output_name}.lib"
        IMPORTED_LOCATION_RELEASE
            "${FORGE_FORSETTI_BINARY_DIR}/src/${component_dir}/Release/${output_name}.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FORGE_FORSETTI_SOURCE_DIR}/include"
    )
    add_dependencies(${target_name} forsetti_external)
endfunction()

_forge_import_forsetti_library(ForgeForsettiCoreImported ForsettiCore ForsettiCore)
add_library(Forsetti::Core ALIAS ForgeForsettiCoreImported)
set_property(TARGET ForgeForsettiCoreImported PROPERTY
    INTERFACE_LINK_LIBRARIES nlohmann_json::nlohmann_json)

_forge_import_forsetti_library(ForgeForsettiPlatformImported ForsettiPlatform ForsettiPlatform)
add_library(Forsetti::Platform ALIAS ForgeForsettiPlatformImported)
set_property(TARGET ForgeForsettiPlatformImported PROPERTY
    INTERFACE_LINK_LIBRARIES "Forsetti::Core;advapi32;bcrypt;crypt32;winhttp")

_forge_import_forsetti_library(ForgeForsettiHostTemplateImported ForsettiHostTemplate ForsettiHostTemplate)
add_library(Forsetti::HostTemplate ALIAS ForgeForsettiHostTemplateImported)
set_property(TARGET ForgeForsettiHostTemplateImported PROPERTY
    INTERFACE_LINK_LIBRARIES "Forsetti::Core;Forsetti::Platform")
