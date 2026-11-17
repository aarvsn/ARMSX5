# ARMSX5 native test suites (GoogleTest).
#
# The homebrew test ELFs are built from tools/test_elfs/*.S with the host C
# compiler in freestanding mode. They are original ARMSX5 code, statically
# linked, and call the emulated kernel through our documented test ABI.

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_elfs)

function(add_test_elf name)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/test_elfs/${name}.elf
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -nostartfiles
                -fno-stack-protector -fno-pie -no-pie
                -o ${CMAKE_BINARY_DIR}/test_elfs/${name}.elf
                ${ARMSX5_ROOT}/tools/test_elfs/${name}.S
        DEPENDS ${ARMSX5_ROOT}/tools/test_elfs/${name}.S
        COMMENT "Building homebrew test ELF ${name}.elf"
        VERBATIM)
endfunction()

add_test_elf(hello)
add_test_elf(sum)
add_test_elf(callret)
add_test_elf(simdavx)
add_custom_target(armsx5_test_elfs ALL
    DEPENDS ${CMAKE_BINARY_DIR}/test_elfs/hello.elf
            ${CMAKE_BINARY_DIR}/test_elfs/sum.elf
            ${CMAKE_BINARY_DIR}/test_elfs/callret.elf
            ${CMAKE_BINARY_DIR}/test_elfs/simdavx.elf)

function(armsx5_add_test name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE ${ARMSX5_WARNING_FLAGS})
    target_link_libraries(${name} PRIVATE gtest_main armsx5_system armsx5_gpu armsx5_input
                                          armsx5_audio)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

armsx5_add_test(armsx5_tests_core_memory ${CMAKE_CURRENT_SOURCE_DIR}/memory/core_memory_tests.cpp)
armsx5_add_test(armsx5_tests_loader ${CMAKE_CURRENT_SOURCE_DIR}/loader/loader_tests.cpp)
armsx5_add_test(armsx5_tests_cpu ${CMAKE_CURRENT_SOURCE_DIR}/cpu/cpu_tests.cpp)
armsx5_add_test(armsx5_tests_system ${CMAKE_CURRENT_SOURCE_DIR}/system/system_tests.cpp)
armsx5_add_test(armsx5_tests_gpu ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gpu_tests.cpp)

add_executable(armsx5_tests_integration ${CMAKE_CURRENT_SOURCE_DIR}/integration/integration_tests.cpp)
target_compile_options(armsx5_tests_integration PRIVATE ${ARMSX5_WARNING_FLAGS})
target_compile_definitions(armsx5_tests_integration PRIVATE
    ARMSX5_TEST_ELFS_DIR="${CMAKE_BINARY_DIR}/test_elfs")
target_link_libraries(armsx5_tests_integration PRIVATE gtest_main armsx5_system)
add_dependencies(armsx5_tests_integration armsx5_test_elfs)
add_test(NAME armsx5_tests_integration COMMAND armsx5_tests_integration)

# armsx5-runner: host CLI for running test ELFs (development diagnostics).
add_executable(armsx5-runner ${ARMSX5_ROOT}/tools/armsx5-runner.cpp)
target_compile_options(armsx5-runner PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5-runner PRIVATE armsx5_system)
