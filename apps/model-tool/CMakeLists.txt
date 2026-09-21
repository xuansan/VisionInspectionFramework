add_executable(vision-model-tool main.cpp)
vision_demo_resources(vision-model-tool)
target_link_libraries(vision-model-tool PRIVATE vision_inference vision_json vision_capture vision_replay)
target_compile_definitions(vision-model-tool PRIVATE
    "VISION_ONNX_MANIFEST=\"$<TARGET_FILE_DIR:vision-onnx-algorithm>/onnx-algorithm.json\"")
add_dependencies(vision-model-tool vision-onnx-algorithm)
vision_deploy_ort(vision-model-tool)
vision_configure_target(vision-model-tool)
