add_library(armsx5_audio
    src/audio_engine.cpp
)

target_include_directories(armsx5_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(armsx5_audio PRIVATE ${ARMSX5_WARNING_FLAGS})
target_link_libraries(armsx5_audio PUBLIC armsx5_core)

if(ANDROID)
    target_link_libraries(armsx5_audio PUBLIC aaudio)
endif()
