add_executable(vision-workstation main.cpp)
target_link_libraries(vision-workstation PRIVATE vision_application vision_serialization Qt6::Core Qt6::Gui Qt6::Widgets)
vision_configure_target(vision-workstation)
if(VISION_BUILD_LOCAL_IPC)
    target_sources(vision-workstation PRIVATE pipeline.cpp model_tool.cpp)
    add_dependencies(vision-workstation vision-model-tool vision-frame-archive vision-sqlite-output vision-file-output vision-durable-output-agent)
    target_compile_definitions(vision-workstation PRIVATE
        "VISION_MODEL_TOOL=\"$<TARGET_FILE:vision-model-tool>\""
        "VISION_SQLITE_MANIFEST=\"$<TARGET_FILE_DIR:vision-sqlite-output>/sqlite-output.json\""
        "VISION_FILE_MANIFEST=\"$<TARGET_FILE_DIR:vision-file-output>/file-output.json\""
        "VISION_FRAME_ARCHIVE=\"$<TARGET_FILE:vision-frame-archive>\""
        "VISION_DURABLE_AGENT=\"$<TARGET_FILE:vision-durable-output-agent>\""
        "VISION_MODEL_CONFIG=\"${PROJECT_SOURCE_DIR}/examples/models/detection.json\""
        "VISION_MODEL_IMAGE=\"${PROJECT_SOURCE_DIR}/examples/models/black.pgm\"")
    target_compile_definitions(vision-workstation PRIVATE VISION_PIPELINE_UI)
    vision_demo_resources(vision-workstation)
endif()
if(WIN32)
    foreach(component IN ITEMS Core Gui Widgets)
        add_custom_command(TARGET vision-workstation POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::${component}>"
                "$<TARGET_FILE_DIR:vision-workstation>")
    endforeach()
    foreach(plugin IN ITEMS QWindowsIntegrationPlugin QOffscreenIntegrationPlugin)
        if(NOT TARGET Qt6::${plugin})
            message(FATAL_ERROR "Required Qt platform plugin not found: ${plugin}")
        endif()
        add_custom_command(TARGET vision-workstation POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vision-workstation>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::${plugin}>"
                "$<TARGET_FILE_DIR:vision-workstation>/platforms")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/qt.conf"
        CONTENT "[Paths]\nPlugins=.\n")
endif()
