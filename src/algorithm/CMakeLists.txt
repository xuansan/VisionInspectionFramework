add_library(vision_algorithm STATIC brightness.cpp)
target_include_directories(vision_algorithm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_algorithm PUBLIC vision_contracts)
vision_configure_target(vision_algorithm)
