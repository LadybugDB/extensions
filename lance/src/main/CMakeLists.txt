add_library(lance_extension_main
        OBJECT
        lance_extension.cpp)

set(LANCE_EXTENSION_OBJECT_FILES
        ${LANCE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lance_extension_main>
        PARENT_SCOPE)
