add_library(vision-webhook-output SHARED output.cpp)
target_link_libraries(vision-webhook-output PRIVATE vision_plugin_sdk vision_serialization vision_json CURL::libcurl)
vision_configure_target(vision-webhook-output)
add_custom_command(TARGET vision-webhook-output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-webhook-output>/webhook-output.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-webhook-output>/LICENSE.txt")
