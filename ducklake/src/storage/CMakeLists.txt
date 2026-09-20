add_library(lbug_ducklake_storage
        OBJECT
        ducklake_storage.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/catalog/duckdb_catalog.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/function/duckdb_scan.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/function/clear_cache.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_type_converter.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/catalog/duckdb_table_catalog_entry.cpp
)

set(DUCKLAKE_EXTENSION_OBJECT_FILES
        ${DUCKLAKE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_ducklake_storage>
        PARENT_SCOPE)
