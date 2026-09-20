add_library(lbug_ducklake_connector
        OBJECT
        ducklake_connector.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_connector.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_result_converter.cpp)

set(DUCKLAKE_EXTENSION_OBJECT_FILES
        ${DUCKLAKE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_ducklake_connector>
        PARENT_SCOPE)
