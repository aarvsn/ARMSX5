add_library(armsx5_gpu
    src/pm4/pm4.cpp
    src/resource.cpp
    src/shader/rdna2.cpp
    src/shader/spirv.cpp
    src/vulkan/device_info.cpp
)

target_include_directories(armsx5_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_gpu PRIVATE ${ARMSX5_WARNING_FLAGS})

# Vendored Khronos Vulkan headers (Apache-2.0), include-only, no linkage.
target_include_directories(armsx5_gpu PUBLIC
    ${ARMSX5_ROOT}/externals/vulkan-headers/include)

target_link_libraries(armsx5_gpu PUBLIC armsx5_core ${CMAKE_DL_LIBS})
