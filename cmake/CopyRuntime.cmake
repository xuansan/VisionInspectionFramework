# Serialize shared runtime deployment (several test executables share one folder).
set(source "${CMAKE_ARGV3}")
set(destination "${CMAKE_ARGV4}")
if(IS_DIRECTORY "${destination}")
    set(directory "${destination}")
else()
    get_filename_component(directory "${destination}" DIRECTORY)
endif()
file(LOCK "${directory}/.runtime-copy.lock" GUARD PROCESS TIMEOUT 30)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${destination}"
    RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Runtime copy failed: ${source} -> ${destination}")
endif()
