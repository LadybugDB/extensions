if (${BUILD_EXTENSION_TESTS})
    include(GoogleTest)
    foreach(cdc_test cdc_wal_test cdc_op_resolver_test cdc_sync_test)
        add_executable(sqlite_${cdc_test} ${cdc_test}.cpp)
        target_include_directories(sqlite_${cdc_test} PRIVATE
            ${PROJECT_SOURCE_DIR}/extension/sqlite/src/include
            ${PROJECT_SOURCE_DIR}/extension/sqlite/third_party/sqlite)
        target_link_libraries(sqlite_${cdc_test} PRIVATE
            sqlite3_vendor
            lbug_sqlite_cdc
            gtest gtest_main)
        gtest_discover_tests(sqlite_${cdc_test} DISCOVERY_MODE PRE_TEST)
    endforeach()
    add_lbug_test(sqlite_sync_api_test sync_api_test.cpp)
    target_include_directories(sqlite_sync_api_test PRIVATE
        ${PROJECT_SOURCE_DIR}/extension/sqlite/third_party/sqlite)
    target_link_libraries(sqlite_sync_api_test PRIVATE sqlite3_vendor)
endif ()
