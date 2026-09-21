add_library(vision-modbus-device SHARED device.cpp)
target_link_libraries(vision-modbus-device PRIVATE vision_plugin_sdk vision_modbus vision_contracts vision_json bcrypt)
vision_configure_target(vision-modbus-device)
add_custom_command(TARGET vision-modbus-device POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-modbus-device>/modbus-device.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-modbus-device>/LICENSE.txt")
