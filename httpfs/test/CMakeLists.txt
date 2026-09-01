if (${BUILD_EXTENSION_TESTS})
    add_lbug_test(httpfs_xetfs_test xetfs_test.cpp)
    target_link_libraries(httpfs_xetfs_test PRIVATE httpfs_extension_source ${OPENSSL_LIBRARIES})

    add_lbug_test(httpfs_httpfs_test httpfs_test.cpp)
    target_link_libraries(httpfs_httpfs_test PRIVATE httpfs_extension_source ${OPENSSL_LIBRARIES})

    # httpfs_extension_source is compiled with CPPHTTPLIB_OPENSSL_SUPPORT, and
    # the test TUs include httplib.h as well. Mixing the two variants in one
    # binary is an ODR violation (e.g. httplib::Socket gains an `SSL*` member
    # when the flag is defined), which manifests as SIGSEGV inside httplib
    # client code depending on the linker's choice of inline definitions.
    # Keep the flags consistent across all TUs that include httplib.h.
    target_compile_definitions(httpfs_xetfs_test PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_compile_definitions(httpfs_httpfs_test PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
endif ()
