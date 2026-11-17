add_library(armsx5_memory
    src/guest_memory.cpp
)

target_include_directories(armsx5_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_memory PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_memory PUBLIC armsx5_core)
