add_subdirectory(config)

add_library(lbug_algo_function
        OBJECT
        component_ids.cpp
        strongly_connected_components.cpp
        strongly_connected_components_kosaraju.cpp
        weakly_connected_components.cpp
        page_rank.cpp
        gds_page_rank.cpp
        k_core_decomposition.cpp
        louvain.cpp
        spanning_forest.cpp
        )

# gds_page_rank.cpp includes NetworKit + Arrow headers (Arrow pulls in <omp.h>). Linking the
# imported targets to the object library propagates their compile flags + include dirs to it.
target_link_libraries(lbug_algo_function PRIVATE Arrow::arrow_shared OpenMP::OpenMP_CXX)

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_algo_function>
        PARENT_SCOPE)
