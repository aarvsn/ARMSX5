cmake_minimum_required(VERSION 3.22)
project(armsx5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ARMSX5_BUILD_TESTS "Build ARMSX5 native unit tests (host)" ON)
option(ARMSX5_ANDROID "Building the ARMSX5 native core for Android (NDK)" OFF)

if(ARMSX5_ANDROID)
    set(ARMSX5_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

set(ARMSX5_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(ARMSX5_WARNING_FLAGS -Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

add_subdirectory(native/core)
add_subdirectory(native/memory)
add_subdirectory(native/loader)
add_subdirectory(native/cpu)
add_subdirectory(native/system)
add_subdirectory(native/gpu)
add_subdirectory(native/audio)
add_subdirectory(native/input)
add_subdirectory(native/platform)

if(ARMSX5_BUILD_TESTS)
    if(EXISTS ${ARMSX5_ROOT}/externals/googletest/CMakeLists.txt)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory(externals/googletest EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
        FetchContent_MakeAvailable(googletest)
    endif()
    enable_testing()
    add_subdirectory(tests)
endif()
