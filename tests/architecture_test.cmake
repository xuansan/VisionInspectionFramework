if(NOT DEFINED PROTECTED_TARGET)
    set(PROTECTED_TARGET vision_application)
endif()
if(NOT DEFINED FORBIDDEN_DEPENDENCY)
    set(FORBIDDEN_DEPENDENCY Qt6::Core)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE}" -B "${BINARY}"
    "-DPROTECTED_TARGET=${PROTECTED_TARGET}" "-DFORBIDDEN_DEPENDENCY=${FORBIDDEN_DEPENDENCY}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "Architecture boundary: ${PROTECTED_TARGET} cannot link ${FORBIDDEN_DEPENDENCY}")
    message(FATAL_ERROR "Expected the specific dependency guard rejection; got ${result}: ${output} ${error}")
endif()
