add_subdirectory(mqtt)
function(vision_mq_plugin name sdk library)
    add_library(vision-${name}-output SHARED output.cpp)
    target_compile_definitions(vision-${name}-output PRIVATE NOMINMAX)
    target_include_directories(vision-${name}-output SYSTEM PRIVATE "${sdk}/include")
    target_link_libraries(vision-${name}-output PRIVATE vision_plugin_sdk vision_serialization vision_json "${library}" ws2_32 crypt32 secur32 bcrypt)
    vision_configure_target(vision-${name}-output)
    add_custom_command(TARGET vision-${name}-output POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-${name}-output>/${name}-output.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-${name}-output>/LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/dependencies/licenses/${name}" "$<TARGET_FILE_DIR:vision-${name}-output>/${name}-notices")
endfunction()
add_subdirectory(kafka)
add_subdirectory(rabbitmq)
add_subdirectory(nats-jetstream)
