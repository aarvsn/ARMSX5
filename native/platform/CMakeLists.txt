add_library(armsx5_platform
    src/exec_arena.cpp
)

target_include_directories(armsx5_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_platform PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_platform PUBLIC armsx5_core)
