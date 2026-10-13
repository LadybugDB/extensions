if (${BUILD_EXTENSION_TESTS})
    add_executable(sqlite_cdc_test cdc_wal_test.cpp)
    target_include_directories(sqlite_cdc_test PRIVATE
        ${PROJECT_SOURCE_DIR}/extension/sqlite/src/include
        ${PROJECT_SOURCE_DIR}/extension/sqlite/third_party/sqlite)
    target_link_libraries(sqlite_cdc_test PRIVATE
        sqlite3_vendor
        lbug_sqlite_cdc
        gtest gtest_main)
    include(GoogleTest)
    gtest_discover_tests(sqlite_cdc_test DISCOVERY_MODE PRE_TEST)
endif ()
