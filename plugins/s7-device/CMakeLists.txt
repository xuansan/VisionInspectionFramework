option(VISION_BUILD_S7_DEVICE "Build optional Snap7 S7 communication adapter" OFF)
set(VISION_SNAP7_ROOT "${PROJECT_SOURCE_DIR}/开发环境/源码/snap7" CACHE PATH "Snap7 source/build root")

if(VISION_BUILD_S7_DEVICE)
    set(SNAP7_INCLUDE "${VISION_SNAP7_ROOT}/release/wrappers/c-cpp")
    set(SNAP7_LIB "${VISION_SNAP7_ROOT}/build/bin/win64/snap7.lib")
    if(NOT EXISTS "${SNAP7_INCLUDE}/snap7.h" OR NOT EXISTS "${SNAP7_LIB}")
        message(FATAL_ERROR "Snap7 x64 SDK missing; set VISION_SNAP7_ROOT or disable VISION_BUILD_S7_DEVICE")
    endif()
    add_library(vision-s7-device SHARED device.cpp)
    target_include_directories(vision-s7-device PRIVATE "${SNAP7_INCLUDE}")
    target_link_libraries(vision-s7-device PRIVATE vision_plugin_sdk vision_contracts vision_json bcrypt "${SNAP7_LIB}")
    vision_configure_target(vision-s7-device)
    add_custom_command(TARGET vision-s7-device POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
            "$<TARGET_FILE_DIR:vision-s7-device>/s7-device.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VISION_SNAP7_ROOT}/lgpl-3.0.txt"
            "$<TARGET_FILE_DIR:vision-s7-device>/LGPL-3.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VISION_SNAP7_ROOT}/gpl.txt" "$<TARGET_FILE_DIR:vision-s7-device>/GPL-3.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-s7-device>/LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/SNAP7-NOTICE.txt" "$<TARGET_FILE_DIR:vision-s7-device>/SNAP7-NOTICE.txt")
endif()
