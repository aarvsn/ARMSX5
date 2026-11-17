add_library(armsx5_input
    src/controller.cpp
)

target_include_directories(armsx5_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_input PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_input PUBLIC armsx5_core)
