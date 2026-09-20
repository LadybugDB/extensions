add_library(lbug_ducklake_options
        OBJECT
        ducklake_options.cpp)

set(DUCKLAKE_EXTENSION_OBJECT_FILES
        ${DUCKLAKE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_ducklake_options>
        PARENT_SCOPE)
