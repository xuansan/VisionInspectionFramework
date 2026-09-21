add_library(vision_modbus STATIC client.cpp)
target_include_directories(vision_modbus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_modbus PRIVATE ws2_32)
vision_configure_target(vision_modbus)
