if(WIN32)
    set(VISION_NATIVE_ROOT "${PROJECT_SOURCE_DIR}/开发环境/SDK/native")
    add_library(vision_sqlite STATIC IMPORTED)
    set_target_properties(vision_sqlite PROPERTIES IMPORTED_LOCATION "${VISION_NATIVE_ROOT}/lib/sqlite3.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${VISION_NATIVE_ROOT}/include")
    set(CURL_DIR "${VISION_NATIVE_ROOT}/lib/cmake/CURL")
    find_package(CURL CONFIG REQUIRED)
endif()
