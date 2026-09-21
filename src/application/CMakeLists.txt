configure_file(build_version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/build_version.hpp" @ONLY)
add_library(vision_application STATIC build_info.cpp demo.cpp)
target_include_directories(vision_application
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(vision_application PUBLIC vision_contracts vision_inspection vision_runtime)
vision_configure_target(vision_application)
