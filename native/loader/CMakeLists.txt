add_library(armsx5_loader
    src/elf_loader.cpp
)

target_include_directories(armsx5_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_loader PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_loader PUBLIC armsx5_memory)
