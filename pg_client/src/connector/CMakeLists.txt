add_library(lbug_pg_client_connector
        OBJECT
        pg_client_connector.cpp)


set(PG_CLIENT_EXTENSION_OBJECT_FILES
        ${PG_CLIENT_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_pg_client_connector>
        PARENT_SCOPE)
