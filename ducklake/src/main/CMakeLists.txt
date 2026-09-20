add_library(ducklake_extension_main
        OBJECT
        ducklake_extension.cpp)

set(DUCKLAKE_EXTENSION_OBJECT_FILES
        ${DUCKLAKE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_extension_main>
        PARENT_SCOPE)
