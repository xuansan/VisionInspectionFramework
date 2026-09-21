if(WIN32)
    add_executable(vision-worker-host main.cpp)
    target_link_libraries(vision-worker-host PRIVATE vision_plugin_runtime vision_runtime vision_ipc_qt)
    vision_configure_target(vision-worker-host)
    foreach(component IN ITEMS Core Network)
        add_custom_command(TARGET vision-worker-host POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::${component}>"
            "$<TARGET_FILE_DIR:vision-worker-host>")
    endforeach()
endif()
