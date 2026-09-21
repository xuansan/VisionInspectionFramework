add_library(vision_capture STATIC source.cpp)
target_include_directories(vision_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_capture PUBLIC vision_contracts)
vision_configure_target(vision_capture)
