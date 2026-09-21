add_library(vision_contracts INTERFACE)
target_include_directories(vision_contracts INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(vision_contracts INTERFACE cxx_std_20)
