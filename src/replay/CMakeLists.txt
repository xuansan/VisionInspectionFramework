add_library(vision_replay STATIC assets.cpp)
target_include_directories(vision_replay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_replay PUBLIC vision_contracts PRIVATE vision_inference vision_capture vision_json)
vision_configure_target(vision_replay)
