add_library(lbug_adbc_connector
        OBJECT
        adbc_connector.cpp)

set(ADBC_EXTENSION_OBJECT_FILES
        ${ADBC_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_adbc_connector>
        PARENT_SCOPE)
