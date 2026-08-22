if (WIN32 OR BUILD_STATIC_EXTENSION)
    set(DuckDB_USE_STATIC_LIBS ON)
else ()
    set(DuckDB_USE_STATIC_LIBS OFF)
endif ()
find_package(DuckDB REQUIRED)

include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/extension/httpfs/src/include  # For S3 configuration
        src/include
        ${DuckDB_INCLUDE_DIRS})

add_subdirectory(src/catalog)
add_subdirectory(src/connector)
add_subdirectory(src/main)
add_subdirectory(src/function)
add_subdirectory(src/storage)
add_subdirectory(src/installer)
add_subdirectory(src/loader)

build_extension_lib(${BUILD_STATIC_EXTENSION} "duckdb")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        ${DuckDB_LIBRARIES})
if (DuckDB_USE_STATIC_LIBS)
    # Statically-linked duckdb requires its generated extension loader archive,
    # which provides the init hooks for duckdb's bundled extensions, plus the
    # archives of those bundled extensions themselves. Order matters: the
    # loader references symbols from both duckdb_static and the extension
    # archives, so it must be linked in between.
    if (TARGET duckdb_generated_extension_loader)
        set(DUCKDB_STATIC_EXTRA_LIBS duckdb_generated_extension_loader)
        foreach (duckdb_ext IN LISTS DuckDB_EXTENSIONS)
            if (TARGET "${duckdb_ext}_extension")
                list(APPEND DUCKDB_STATIC_EXTRA_LIBS "${duckdb_ext}_extension")
            endif ()
        endforeach ()
        target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
                PRIVATE
                ${DUCKDB_STATIC_EXTRA_LIBS})
    else ()
        message(WARNING "duckdb static libs are used but the duckdb_generated_extension_loader "
                "target was not exported by the DuckDB package - the build may fail to link")
    endif ()
endif ()
