add_library(lbug_adbc_function
        OBJECT
        adbc_scan.cpp
        ${PROJECT_SOURCE_DIR}/src/common/arrow/arrow_array_scan.cpp
        ${PROJECT_SOURCE_DIR}/src/common/arrow/arrow_converter.cpp
        ${PROJECT_SOURCE_DIR}/src/common/arrow/arrow_null_mask_tree.cpp
        ${PROJECT_SOURCE_DIR}/src/common/arrow/arrow_type.cpp)

set(ADBC_EXTENSION_OBJECT_FILES
        ${ADBC_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_adbc_function>
        PARENT_SCOPE)
