cmake_minimum_required(VERSION 3.21)

project(WyVehicleAssetBridge VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WyVehicleAssetBridge requires the 64-bit MSVC toolchain for 3ds Max 2025.")
endif()

set(MAX_SDK_PATH "" CACHE PATH "3ds Max 2025 SDK root (contains include/max.h)")
if(NOT EXISTS "${MAX_SDK_PATH}/include/max.h")
    message(FATAL_ERROR "Set MAX_SDK_PATH to the 3ds Max 2025 SDK root containing include/max.h. The 3ds Max application install alone does not include the SDK.")
endif()

# Autodesk's release SDK libraries use the release DLL runtime, also when
# debugging this plug-in with RelWithDebInfo.
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(is_multi_config)
    set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "Supported configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    message(FATAL_ERROR "Use Release or RelWithDebInfo with the release 3ds Max SDK libraries.")
endif()

set(MAX_SDK_LIBRARY_DIR "${MAX_SDK_PATH}/lib/x64/Release" CACHE PATH "3ds Max SDK release import library directory")
foreach(sdk_library core maxutil gup)
    if(NOT EXISTS "${MAX_SDK_LIBRARY_DIR}/${sdk_library}.lib")
        message(FATAL_ERROR "Missing ${MAX_SDK_LIBRARY_DIR}/${sdk_library}.lib. Check MAX_SDK_PATH or MAX_SDK_LIBRARY_DIR.")
    endif()
endforeach()

add_library(WyVehicleAssetBridge SHARED Source/WyVehicleAssetBridge.cpp)
# 3ds Max 2025 uses C++20 (Autodesk CY24 / 2025 VFX platform alignment).
target_compile_features(WyVehicleAssetBridge PRIVATE cxx_std_20)
target_compile_definitions(WyVehicleAssetBridge PRIVATE UNICODE _UNICODE _ITERATOR_DEBUG_LEVEL=0)
# A source change must invalidate MSBuild objects even if a file restore preserved its timestamp.
if(WY_VEHICLE_SOURCE_FINGERPRINT)
    target_compile_definitions(WyVehicleAssetBridge PRIVATE WY_VEHICLE_SOURCE_FINGERPRINT="${WY_VEHICLE_SOURCE_FINGERPRINT}")
endif()
target_compile_options(WyVehicleAssetBridge PRIVATE /W4 /permissive- /utf-8)
target_include_directories(WyVehicleAssetBridge SYSTEM PRIVATE "${MAX_SDK_PATH}/include")
target_link_libraries(WyVehicleAssetBridge PRIVATE
    "${MAX_SDK_LIBRARY_DIR}/core.lib"
    "${MAX_SDK_LIBRARY_DIR}/maxutil.lib"
    "${MAX_SDK_LIBRARY_DIR}/gup.lib"
)
set_target_properties(WyVehicleAssetBridge PROPERTIES
    PREFIX ""
    SUFFIX ".gup"
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

# Install into the package staging directory supplied by the root build script.
install(TARGETS WyVehicleAssetBridge RUNTIME DESTINATION Contents/Win64)
install(FILES PackageContents.xml DESTINATION .)
