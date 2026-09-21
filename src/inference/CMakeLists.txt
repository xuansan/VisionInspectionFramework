add_library(vision_inference STATIC engine.cpp assets.cpp)
target_include_directories(vision_inference PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_inference PUBLIC vision_contracts PRIVATE vision_ort vision_json bcrypt)
vision_configure_target(vision_inference)
