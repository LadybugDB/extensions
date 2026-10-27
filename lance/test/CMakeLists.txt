if (${BUILD_EXTENSION_TESTS})
    add_lbug_test(lance_utils_test lance_utils_test.cpp)
    target_include_directories(lance_utils_test PRIVATE
        ${PROJECT_SOURCE_DIR}/extension/lance/src/include)
endif ()
