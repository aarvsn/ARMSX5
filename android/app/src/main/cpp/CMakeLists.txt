# ARMSX5 Android JNI library. Builds the full native core plus the JNI glue
# into libarmsx5_jni.so, packaged for arm64-v8a.
cmake_minimum_required(VERSION 3.22)
project(armsx5-android LANGUAGES CXX)

# cpp -> main -> src -> app -> android -> ARMSX5 root
set(ARMSX5_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(ARMSX5_ANDROID ON)
set(ARMSX5_WARNING_FLAGS -Wall -Wextra -Wno-unused-parameter)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_subdirectory(${ARMSX5_ROOT}/native/core core)
add_subdirectory(${ARMSX5_ROOT}/native/memory memory)
add_subdirectory(${ARMSX5_ROOT}/native/loader loader)
add_subdirectory(${ARMSX5_ROOT}/native/cpu cpu)
add_subdirectory(${ARMSX5_ROOT}/native/platform platform)
add_subdirectory(${ARMSX5_ROOT}/native/system system)
add_subdirectory(${ARMSX5_ROOT}/native/gpu gpu)
add_subdirectory(${ARMSX5_ROOT}/native/audio audio)
add_subdirectory(${ARMSX5_ROOT}/native/input input)

add_library(armsx5_jni SHARED armsx5_jni.cpp)
target_compile_options(armsx5_jni PRIVATE ${ARMSX5_WARNING_FLAGS})
target_include_directories(armsx5_jni PRIVATE
    ${ARMSX5_ROOT}/native/core/include
    ${ARMSX5_ROOT}/native/memory/include
    ${ARMSX5_ROOT}/native/loader/include
    ${ARMSX5_ROOT}/native/cpu/include
    ${ARMSX5_ROOT}/native/platform/include
    ${ARMSX5_ROOT}/native/system/include
    ${ARMSX5_ROOT}/native/gpu/include
    ${ARMSX5_ROOT}/native/audio/include
    ${ARMSX5_ROOT}/native/input/include)
target_link_libraries(armsx5_jni PRIVATE
    armsx5_system armsx5_gpu armsx5_input armsx5_audio armsx5_core)

set_target_properties(armsx5_jni PROPERTIES LINK_OPTIONS "-Wl,--no-undefined")
