add_library(lbug_pg_client_catalog
        OBJECT
        pg_client_catalog.cpp
        pg_client_table_catalog_entry.cpp)

set(PG_CLIENT_EXTENSION_OBJECT_FILES
        ${PG_CLIENT_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_pg_client_catalog>
        PARENT_SCOPE)
