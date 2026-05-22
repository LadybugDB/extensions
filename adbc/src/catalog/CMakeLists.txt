add_library(lbug_adbc_catalog
        OBJECT
        adbc_catalog.cpp
        adbc_table_catalog_entry.cpp)

set(ADBC_EXTENSION_OBJECT_FILES
        ${ADBC_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_adbc_catalog>
        PARENT_SCOPE)
