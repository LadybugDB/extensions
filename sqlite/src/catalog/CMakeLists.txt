add_library(lbug_sqlite_catalog
        OBJECT
        sqlite_catalog.cpp
        sqlite_table_catalog_entry.cpp)

set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_catalog>
        PARENT_SCOPE)
