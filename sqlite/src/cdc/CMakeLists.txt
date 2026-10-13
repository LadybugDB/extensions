add_library(lbug_sqlite_cdc
        OBJECT
        wal_tailer.cpp
        btree_page_decoder.cpp)

set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_cdc>
        PARENT_SCOPE)
