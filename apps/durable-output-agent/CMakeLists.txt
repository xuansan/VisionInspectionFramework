add_executable(vision-durable-output-agent main.cpp)
vision_demo_resources(vision-durable-output-agent)
target_link_libraries(vision-durable-output-agent PRIVATE vision_storage vision_inference vision_json)
vision_configure_target(vision-durable-output-agent)
