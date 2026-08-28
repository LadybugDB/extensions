add_library(lbug_iceberg_options
        OBJECT
        iceberg_options.cpp)

set(ICEBERG_EXTENSION_OBJECT_FILES
        ${ICEBERG_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_iceberg_options>
        PARENT_SCOPE)
