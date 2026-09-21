add_executable(vision-cli main.cpp)
target_link_libraries(vision-cli PRIVATE vision_application vision_serialization vision_ipc)
vision_configure_target(vision-cli)
