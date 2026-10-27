add_library(lbug_lance_function
        OBJECT
        lance_scan.cpp)

set(LANCE_EXTENSION_OBJECT_FILES
        ${LANCE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_lance_function>
        PARENT_SCOPE)
