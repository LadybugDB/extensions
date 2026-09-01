if (${BUILD_EXTENSION_TESTS})
    add_lbug_test(httpfs_xetfs_test xetfs_test.cpp)
    target_link_libraries(httpfs_xetfs_test PRIVATE httpfs_extension_source ${OPENSSL_LIBRARIES})

    add_lbug_test(httpfs_httpfs_test httpfs_test.cpp)
    target_link_libraries(httpfs_httpfs_test PRIVATE httpfs_extension_source ${OPENSSL_LIBRARIES})
endif ()
