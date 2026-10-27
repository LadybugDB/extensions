if (NOT WIN32)
    add_library(lance_installer
            SHARED
            lance_install_func.cpp
            ${PROJECT_SOURCE_DIR}/extension/duckdb/src/installer/duckdb_installer.cpp)
    set_extension_properties(lance_installer lance_installer lance)
endif ()

if (APPLE)
    set_apple_dynamic_lookup(lance_installer)
endif ()
