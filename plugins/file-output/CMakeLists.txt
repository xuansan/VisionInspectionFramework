add_library(vision-file-output SHARED output.cpp)
target_link_libraries(vision-file-output PRIVATE vision_plugin_sdk vision_serialization vision_inference vision_json)
vision_configure_target(vision-file-output)
add_custom_command(TARGET vision-file-output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-file-output>/file-output.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-file-output>/LICENSE.txt")
