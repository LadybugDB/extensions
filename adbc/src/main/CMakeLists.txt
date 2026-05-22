add_library(adbc_extension_main
        OBJECT
        adbc_extension.cpp)

set(ADBC_EXTENSION_OBJECT_FILES
        ${ADBC_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:adbc_extension_main>
        PARENT_SCOPE)
