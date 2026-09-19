add_library(lbug_iceberg_storage
        OBJECT
        iceberg_storage.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/catalog/duckdb_catalog.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/function/duckdb_scan.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/function/clear_cache.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/catalog/duckdb_table_catalog_entry.cpp
)

set(ICEBERG_EXTENSION_OBJECT_FILES
        ${ICEBERG_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_iceberg_storage>
        PARENT_SCOPE)
