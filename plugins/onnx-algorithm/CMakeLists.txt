add_library(vision-onnx-algorithm SHARED algorithm.cpp)
target_link_libraries(vision-onnx-algorithm PRIVATE vision_plugin_sdk vision_inference vision_frame_transport vision_serialization vision_json)
vision_configure_target(vision-onnx-algorithm)
vision_deploy_ort(vision-onnx-algorithm)
add_custom_command(TARGET vision-onnx-algorithm POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "$<TARGET_FILE_DIR:vision-onnx-algorithm>/onnx-algorithm.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt"
    "$<TARGET_FILE_DIR:vision-onnx-algorithm>/LICENSE.txt")
