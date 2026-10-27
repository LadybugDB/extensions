add_library(lbug_lance_connector
        OBJECT
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_secret_manager.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_connector.cpp
        lance_connector.cpp)

set(LANCE_EXTENSION_OBJECT_FILES
        ${LANCE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_lance_connector>
        PARENT_SCOPE)
