add_library(vision_runtime STATIC resources.cpp supervisor.cpp scheduler.cpp buffer_broker.cpp)
target_include_directories(vision_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_runtime PUBLIC vision_contracts)
vision_configure_target(vision_runtime)
