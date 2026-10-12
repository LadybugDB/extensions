add_library(lbug_sqlite_storage
        OBJECT
        sqlite_storage.cpp)

set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_storage>
        PARENT_SCOPE)
