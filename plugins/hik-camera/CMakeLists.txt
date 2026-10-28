option(VISION_BUILD_HIK_CAMERA "Build optional installed MVS camera adapter" OFF)
set(VISION_MVS_ROOT "C:/Program Files (x86)/MVS/Development" CACHE PATH "Installed MVS development directory")
set(VISION_MVS_RUNTIME "C:/Program Files (x86)/Common Files/MVS/Runtime/Win64_x64" CACHE PATH "Installed MVS x64 runtime directory")
function(vision_hik_target target backend)
    add_library(${target} SHARED camera.cpp ${backend})
    target_link_libraries(${target} PRIVATE vision_plugin_sdk vision_frame_transport vision_serialization vision_json)
    vision_configure_target(${target})
    set(HIK_LIBRARY "${target}.dll")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in" "${CMAKE_CURRENT_BINARY_DIR}/${target}.json" @ONLY)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/${target}.json" "$<TARGET_FILE_DIR:${target}>/${target}.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/plugins/sim-camera/LICENSE.txt" "$<TARGET_FILE_DIR:${target}>/LICENSE.txt")
endfunction()
if(VISION_BUILD_HIK_CAMERA)
    if(NOT EXISTS "${VISION_MVS_ROOT}/Includes/MvCameraControl.h" OR NOT EXISTS "${VISION_MVS_ROOT}/Libraries/win64/MvCameraControl.lib")
        message(FATAL_ERROR "MVS x64 development SDK missing; configure VISION_MVS_ROOT or disable VISION_BUILD_HIK_CAMERA")
    endif()
    vision_hik_target(vision-hik-camera mvs_backend.cpp)
    target_include_directories(vision-hik-camera SYSTEM PRIVATE "${VISION_MVS_ROOT}/Includes")
    target_link_libraries(vision-hik-camera PRIVATE "${VISION_MVS_ROOT}/Libraries/win64/MvCameraControl.lib")
endif()
if(BUILD_TESTING)
    vision_hik_target(vision-hik-fake "${PROJECT_SOURCE_DIR}/tests/hik_fake_backend.cpp")
    add_executable(vision-hik-camera-test
        camera.cpp
        "${PROJECT_SOURCE_DIR}/tests/hik_fake_backend.cpp"
        "${PROJECT_SOURCE_DIR}/tests/hik_contract_test.cpp")
    target_link_libraries(vision-hik-camera-test PRIVATE
        vision_plugin_sdk vision_frame_transport vision_serialization vision_json)
    vision_configure_target(vision-hik-camera-test)
    add_test(NAME capture.hik.fake COMMAND vision-hik-camera-test)
    set_tests_properties(capture.hik.fake PROPERTIES TIMEOUT 10)
endif()
