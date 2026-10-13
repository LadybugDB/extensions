add_library(lbug_sqlite_scan
        OBJECT
        sqlite_scan.cpp)

# NOTE: only the last set() here uses PARENT_SCOPE. A PARENT_SCOPE set
# does not update the local scope, so chaining two of them would drop the
# first library from the link.
set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_scan>)

add_library(lbug_sqlite_sync
        OBJECT
        sync_engine.cpp
        sync_functions.cpp)

set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_sync>
        PARENT_SCOPE)
