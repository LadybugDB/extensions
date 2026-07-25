add_library(lbug_pg_client_main
        OBJECT
        pg_client_extension.cpp)


set(PG_CLIENT_EXTENSION_OBJECT_FILES
        ${PG_CLIENT_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_pg_client_main>
        PARENT_SCOPE)
