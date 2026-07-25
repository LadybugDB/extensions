add_library(lbug_pg_client_function OBJECT pg_client_scan.cpp)

set(PG_CLIENT_EXTENSION_OBJECT_FILES
        ${PG_CLIENT_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_pg_client_function>
        PARENT_SCOPE)
