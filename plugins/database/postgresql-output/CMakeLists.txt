if(NOT VISION_BUILD_POSTGRESQL_OUTPUT)
    return()
endif()
if(NOT EXISTS "${VISION_LIBPQ_ROOT}/lib/libpq.lib")
    message(FATAL_ERROR "Install PostgreSQL binary SDK first; see plugins/database/README.md")
endif()
add_library(vision_libpq SHARED IMPORTED GLOBAL)
set_target_properties(vision_libpq PROPERTIES IMPORTED_IMPLIB "${VISION_LIBPQ_ROOT}/lib/libpq.lib"
    IMPORTED_LOCATION "${VISION_LIBPQ_ROOT}/bin/libpq.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${VISION_LIBPQ_ROOT}/include")
vision_database_plugin(postgresql vision_libpq)
vision_deploy_database(vision-postgresql-output)
