add_library(armsx5_system
    src/kernel.cpp
    src/vfs.cpp
    src/module_registry.cpp
    src/process.cpp
)

target_include_directories(armsx5_system PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_system PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_system PUBLIC armsx5_loader armsx5_cpu)
