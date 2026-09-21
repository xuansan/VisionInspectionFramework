add_executable(vision-ipc-probe main.cpp)
target_link_libraries(vision-ipc-probe PRIVATE vision_ipc_qt)
vision_configure_target(vision-ipc-probe)
if(WIN32)
    foreach(component IN ITEMS Core Network)
        add_custom_command(TARGET vision-ipc-probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::${component}>"
                "$<TARGET_FILE_DIR:vision-ipc-probe>")
    endforeach()
endif()
