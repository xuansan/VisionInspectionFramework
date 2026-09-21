# Locked local SDK. No implicit downloads; worker-only dependency.
set(VISION_ORT_ROOT "${PROJECT_SOURCE_DIR}/开发环境/SDK/ONNXRuntime/onnxruntime-win-x64-1.30.0" CACHE PATH "ONNX Runtime 1.30.0 SDK")
if(WIN32)
    if(NOT EXISTS "${VISION_ORT_ROOT}/lib/onnxruntime.dll")
        message(FATAL_ERROR "ONNX Runtime SDK missing: ${VISION_ORT_ROOT}")
    endif()
    add_library(vision_ort SHARED IMPORTED)
    set_target_properties(vision_ort PROPERTIES IMPORTED_IMPLIB "${VISION_ORT_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${VISION_ORT_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${VISION_ORT_ROOT}/include")
    function(vision_deploy_ort target)
        foreach(binary IN ITEMS onnxruntime.dll onnxruntime_providers_shared.dll)
            add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -P "${PROJECT_SOURCE_DIR}/cmake/CopyRuntime.cmake"
                "${VISION_ORT_ROOT}/lib/${binary}" "$<TARGET_FILE_DIR:${target}>")
        endforeach()
    endfunction()
endif()
