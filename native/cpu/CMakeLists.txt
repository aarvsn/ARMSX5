add_library(armsx5_cpu
    src/x86/decoder.cpp
    src/interpreter.cpp
    src/interpreter_sse.cpp
    src/interpreter_avx.cpp
    src/arm64/encoder.cpp
    src/jit.cpp
)

target_include_directories(armsx5_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_cpu PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_cpu PUBLIC armsx5_memory armsx5_platform)
