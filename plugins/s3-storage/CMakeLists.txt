add_library(vision-s3-storage SHARED storage.cpp)
target_link_libraries(vision-s3-storage PRIVATE vision_plugin_sdk vision_storage vision_serialization vision_inference vision_json)
vision_configure_target(vision-s3-storage)
add_custom_command(TARGET vision-s3-storage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-s3-storage>/s3-storage.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-s3-storage>/LICENSE.txt")
