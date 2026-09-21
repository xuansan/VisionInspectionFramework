add_library(vision_ipc STATIC message.cpp channel.cpp)
target_include_directories(vision_ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_ipc PUBLIC vision_contracts PRIVATE vision_json vision_serialization)
vision_configure_target(vision_ipc)
