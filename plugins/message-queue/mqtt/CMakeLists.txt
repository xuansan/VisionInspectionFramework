option(VISION_BUILD_MQTT_OUTPUT "Build Paho MQTT output (local broker, business ACK)" OFF)
set(VISION_PAHO_ROOT "${PROJECT_SOURCE_DIR}/开发环境/SDK/paho" CACHE PATH "Paho C 1.3.15 static SDK")
if(NOT VISION_BUILD_MQTT_OUTPUT)
    return()
endif()
if(NOT EXISTS "${VISION_PAHO_ROOT}/lib/paho-mqtt3a-static.lib")
    message(FATAL_ERROR "Build Paho C 1.3.15 static Release SDK first; see plugins/message-queue/mqtt/README.md")
endif()
add_library(vision_paho_async STATIC IMPORTED GLOBAL)
set_target_properties(vision_paho_async PROPERTIES IMPORTED_LOCATION "${VISION_PAHO_ROOT}/lib/paho-mqtt3a-static.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${VISION_PAHO_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS PAHO_MQTT_STATIC=1
    INTERFACE_LINK_LIBRARIES "ws2_32;crypt32;rpcrt4")
add_library(vision-mqtt-output SHARED output.cpp)
target_link_libraries(vision-mqtt-output PRIVATE vision_plugin_sdk vision_serialization vision_json vision_paho_async)
vision_configure_target(vision-mqtt-output)
add_custom_command(TARGET vision-mqtt-output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "$<TARGET_FILE_DIR:vision-mqtt-output>/mqtt-output.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:vision-mqtt-output>/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/dependencies/licenses/paho" "$<TARGET_FILE_DIR:vision-mqtt-output>/paho-notices")
