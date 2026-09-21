# Explicit direct dependency allow-lists: transitive dependencies are checked
# when their owning first-party target is checked. No generator expressions
# or raw linker flags are allowed in protected targets.
function(vision_check_architecture)
    foreach(target IN ITEMS vision_contracts vision_inspection vision_runtime vision_application vision_ipc
        vision_frame_transport vision_plugin_sdk vision_plugin_runtime vision_capture vision_algorithm vision_inference vision_replay
        vision_storage vision_modbus)
        if(NOT TARGET ${target})
            continue()
        endif()
        if(target STREQUAL "vision_contracts" OR target STREQUAL "vision_plugin_sdk")
            set(allowed "")
        elseif(target STREQUAL "vision_application")
            set(allowed vision_contracts vision_inspection vision_runtime)
        elseif(target STREQUAL "vision_ipc")
            # Static PRIVATE dependencies appear as exact LINK_ONLY expressions.
            set(allowed vision_contracts vision_json vision_serialization
                "$<LINK_ONLY:vision_json>" "$<LINK_ONLY:vision_serialization>")
        elseif(target STREQUAL "vision_plugin_runtime")
            set(allowed vision_plugin_sdk vision_serialization)
        elseif(target STREQUAL "vision_inference")
            set(allowed vision_contracts vision_ort vision_json bcrypt
                "$<LINK_ONLY:vision_ort>" "$<LINK_ONLY:vision_json>" "$<LINK_ONLY:bcrypt>")
        elseif(target STREQUAL "vision_replay")
            set(allowed vision_contracts vision_inference vision_capture vision_json
                "$<LINK_ONLY:vision_inference>" "$<LINK_ONLY:vision_capture>" "$<LINK_ONLY:vision_json>")
        elseif(target STREQUAL "vision_storage")
            set(allowed vision_contracts vision_sqlite vision_serialization vision_inference vision_json CURL::libcurl
                "$<LINK_ONLY:vision_sqlite>" "$<LINK_ONLY:vision_serialization>" "$<LINK_ONLY:vision_inference>"
                "$<LINK_ONLY:vision_json>" "$<LINK_ONLY:CURL::libcurl>")
        elseif(target STREQUAL "vision_modbus")
            set(allowed ws2_32 "$<LINK_ONLY:ws2_32>")
        else()
            set(allowed vision_contracts)
        endif()
        foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(links ${target} ${property})
            if(links)
                foreach(link IN LISTS links)
                    if(NOT link IN_LIST allowed)
                        message(FATAL_ERROR "Architecture boundary: ${target} cannot link ${link}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endforeach()
endfunction()
