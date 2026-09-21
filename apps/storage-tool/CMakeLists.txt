add_executable(vision-storage-tool main.cpp)
target_link_libraries(vision-storage-tool PRIVATE vision_storage vision_serialization vision_inference vision_json)
vision_configure_target(vision-storage-tool)
