add_library(vision-sim-device SHARED device.cpp)
target_link_libraries(vision-sim-device PRIVATE vision_plugin_sdk vision_contracts vision_json)
vision_configure_target(vision-sim-device)
add_custom_command(TARGET vision-sim-device POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "$<TARGET_FILE_DIR:vision-sim-device>/sim-device.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt"
    "$<TARGET_FILE_DIR:vision-sim-device>/LICENSE.txt")
