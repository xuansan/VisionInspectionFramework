add_library(vision_inspection STATIC inspection.cpp rules.cpp)
target_include_directories(vision_inspection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_inspection PUBLIC vision_contracts)
vision_configure_target(vision_inspection)
