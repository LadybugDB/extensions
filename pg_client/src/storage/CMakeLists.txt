add_library(lbug_pg_client_storage
        OBJECT
        pg_client_storage.cpp)


set(PG_CLIENT_EXTENSION_OBJECT_FILES
        ${PG_CLIENT_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_pg_client_storage>
        PARENT_SCOPE)
