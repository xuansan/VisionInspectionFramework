add_library(vision-sim-camera SHARED camera.cpp)
target_link_libraries(vision-sim-camera PRIVATE vision_plugin_sdk vision_capture vision_frame_transport vision_serialization vision_json bcrypt)
vision_configure_target(vision-sim-camera)
add_custom_command(TARGET vision-sim-camera POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "$<TARGET_FILE_DIR:vision-sim-camera>/sim-camera.json")
add_custom_command(TARGET vision-sim-camera POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt"
    "$<TARGET_FILE_DIR:vision-sim-camera>/LICENSE.txt")
