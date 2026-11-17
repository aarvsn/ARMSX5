add_library(armsx5_core
    src/log.cpp
    src/config.cpp
)

target_include_directories(armsx5_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_core PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_core PUBLIC Threads::Threads)

if(ANDROID)
    target_link_libraries(armsx5_core PUBLIC log)
endif()
