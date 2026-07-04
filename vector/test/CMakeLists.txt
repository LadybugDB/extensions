if (${BUILD_EXTENSION_TESTS})
    add_lbug_test(vector_prepare_test prepare_test.cpp)
    add_lbug_test(vector_hnsw_rel_batch_insert_test hnsw_rel_batch_insert_test.cpp)
endif ()
