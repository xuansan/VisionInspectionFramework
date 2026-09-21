function(vision_demo_resources target)
    target_link_libraries(${target} PRIVATE vision_application_qt)
    add_dependencies(${target} vision-worker-host vision-sim-camera vision-basic-algorithm vision-sim-device vision-test-output)
    target_compile_definitions(${target} PRIVATE
        "VISION_DEMO_HOST=\"$<TARGET_FILE:vision-worker-host>\""
        "VISION_DEMO_CAMERA=\"$<TARGET_FILE_DIR:vision-sim-camera>/sim-camera.json\""
        "VISION_DEMO_ALGORITHM=\"$<TARGET_FILE_DIR:vision-basic-algorithm>/basic-algorithm.json\""
        "VISION_DEMO_DEVICE=\"$<TARGET_FILE_DIR:vision-sim-device>/sim-device.json\""
        "VISION_DEMO_OUTPUT=\"$<TARGET_FILE_DIR:vision-test-output>/test-output.json\"")
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -P "${PROJECT_SOURCE_DIR}/cmake/CopyRuntime.cmake"
        "${PROJECT_SOURCE_DIR}/examples/demo/dual-camera.json" "$<TARGET_FILE_DIR:${target}>/dual-camera.json")
    foreach(component IN ITEMS Core Network)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -P "${PROJECT_SOURCE_DIR}/cmake/CopyRuntime.cmake"
            "$<TARGET_FILE:Qt6::${component}>" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
endfunction()
