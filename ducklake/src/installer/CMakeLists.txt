if (NOT WIN32)
    add_library(ducklake_installer
            SHARED
            ducklake_install_func.cpp
            ${PROJECT_SOURCE_DIR}/extension/duckdb/src/installer/duckdb_installer.cpp)
    set_extension_properties(ducklake_installer ducklake_installer ducklake)
endif ()

if (APPLE)
    set_apple_dynamic_lookup(ducklake_installer)
endif ()
