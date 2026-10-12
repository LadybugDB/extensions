add_library(lbug_sqlite_connector
        OBJECT
        sqlite_connector.cpp)

set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_connector>
        PARENT_SCOPE)
