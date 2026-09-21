add_library(vision_storage STATIC store.cpp s3.cpp)
target_include_directories(vision_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vision_storage PUBLIC vision_contracts PRIVATE vision_sqlite vision_serialization vision_inference vision_json CURL::libcurl)
vision_configure_target(vision_storage)
