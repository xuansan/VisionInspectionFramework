add_library(vision-http-output SHARED output.cpp)
target_include_directories(vision-http-output PRIVATE "${VISION_NATIVE_ROOT}/include")
target_link_libraries(vision-http-output PRIVATE vision_plugin_sdk vision_storage vision_serialization vision_inference vision_json ws2_32 crypt32)
vision_configure_target(vision-http-output)
add_custom_command(TARGET vision-http-output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-http-output>/http-output.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-http-output>/LICENSE.txt")
