add_library(vision-basic-algorithm SHARED algorithm.cpp)
target_link_libraries(vision-basic-algorithm PRIVATE vision_plugin_sdk vision_algorithm vision_frame_transport vision_serialization vision_json)
vision_configure_target(vision-basic-algorithm)
add_custom_command(TARGET vision-basic-algorithm POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "$<TARGET_FILE_DIR:vision-basic-algorithm>/basic-algorithm.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt"
    "$<TARGET_FILE_DIR:vision-basic-algorithm>/LICENSE.txt")
