add_library(lbug_lance_function
        OBJECT
        lance_scan.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_result_converter.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_type_converter.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/function/duckdb_scan.cpp)

set(LANCE_EXTENSION_OBJECT_FILES
        ${LANCE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_lance_function>
        PARENT_SCOPE)
