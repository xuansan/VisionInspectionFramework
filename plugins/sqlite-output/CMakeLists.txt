add_library(vision-sqlite-output SHARED output.cpp)
target_link_libraries(vision-sqlite-output PRIVATE vision_plugin_sdk vision_storage vision_serialization vision_inference vision_json)
vision_configure_target(vision-sqlite-output)
add_custom_command(TARGET vision-sqlite-output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-sqlite-output>/sqlite-output.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-sqlite-output>/LICENSE.txt")
