add_library(vision-test-output SHARED output.cpp)
target_link_libraries(vision-test-output PRIVATE vision_plugin_sdk vision_json)
vision_configure_target(vision-test-output)
add_custom_command(TARGET vision-test-output POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "$<TARGET_FILE_DIR:vision-test-output>/test-output.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt"
    "$<TARGET_FILE_DIR:vision-test-output>/LICENSE.txt")
