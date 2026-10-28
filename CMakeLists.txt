cmake_minimum_required(VERSION 3.25)

project(CoSyncSF VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match the vcpkg x64-windows-static-md triplet: static libraries, dynamic CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

option(COSYNC_BUILD_PLUGIN "Build the SFSE plugin DLL" ON)
option(COSYNC_BUILD_TESTS "Build automated tests" ON)

add_library(cosync_warnings INTERFACE)
target_compile_options(cosync_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
target_compile_definitions(cosync_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

find_package(GameNetworkingSockets CONFIG REQUIRED)

add_subdirectory(src/CoSyncSF.Shared)
add_subdirectory(src/CoSyncSF.Network)
add_subdirectory(src/CoSyncSF.Server)
add_subdirectory(src/CoSyncSF.Client)

if(COSYNC_BUILD_PLUGIN)
    include(cmake/CommonLibSF.cmake)
    add_subdirectory(src/CoSyncSF.Plugin)
endif()

if(COSYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/CoSyncSF.Tests)
endif()
