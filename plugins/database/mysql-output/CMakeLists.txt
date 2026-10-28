if(NOT VISION_BUILD_MYSQL_OUTPUT)
    return()
endif()
if(NOT EXISTS "${VISION_MARIADB_ROOT}/lib/libmariadb.lib")
    message(FATAL_ERROR "Build MariaDB Connector/C first; see plugins/database/README.md")
endif()
add_library(vision_mariadb SHARED IMPORTED GLOBAL)
set_target_properties(vision_mariadb PROPERTIES IMPORTED_IMPLIB "${VISION_MARIADB_ROOT}/lib/libmariadb.lib"
    IMPORTED_LOCATION "${VISION_MARIADB_ROOT}/lib/libmariadb.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${VISION_MARIADB_ROOT}/include/mariadb")
vision_database_plugin(mysql vision_mariadb)
vision_deploy_database(vision-mysql-output)
