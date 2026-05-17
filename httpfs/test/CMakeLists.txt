if (${BUILD_EXTENSION_TESTS})
    add_lbug_test(httpfs_xetfs_test xetfs_test.cpp)
    target_link_libraries(httpfs_xetfs_test PRIVATE httpfs_extension_source ${OPENSSL_LIBRARIES})
endif ()
