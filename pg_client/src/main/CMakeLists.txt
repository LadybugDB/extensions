add_library(lbug_pg_client_main
        OBJECT
        pg_client_extension.cpp)

add_dependencies(lbug_pg_client_main lbug_common)

set(PG_CLIENT_EXTENSION_OBJECT_FILES
        ${PG_CLIENT_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_pg_client_main>
        PARENT_SCOPE)
