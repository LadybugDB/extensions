add_library(lbug_adbc_storage
        OBJECT
        adbc_storage.cpp)

set(ADBC_EXTENSION_OBJECT_FILES
        ${ADBC_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_adbc_storage>
        PARENT_SCOPE)
