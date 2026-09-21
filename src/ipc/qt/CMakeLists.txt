add_library(vision_ipc_qt STATIC local_connection.cpp)
target_include_directories(vision_ipc_qt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_ipc_qt PUBLIC vision_ipc Qt6::Core Qt6::Network)
vision_configure_target(vision_ipc_qt)
