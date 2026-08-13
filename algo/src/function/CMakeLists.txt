add_subdirectory(config)

set(_algo_function_sources
        component_ids.cpp
        strongly_connected_components.cpp
        strongly_connected_components_kosaraju.cpp
        weakly_connected_components.cpp
        page_rank.cpp
        k_core_decomposition.cpp
        louvain.cpp
        spanning_forest.cpp
        )
if (ICEBUG_ENABLED)
    # The gds_*.cpp sources include NetworKit + Arrow headers. Linking the imported targets to
    # the object library propagates their compile flags + include dirs to it.
    list(APPEND _algo_function_sources gds_csr_bridge.cpp gds_page_rank.cpp gds_node2vec.cpp
            gds_louvain.cpp gds_leiden.cpp gds_ppr.cpp)
endif ()

add_library(lbug_algo_function
        OBJECT
        ${_algo_function_sources})

if (ICEBUG_ENABLED)
    target_link_libraries(lbug_algo_function PRIVATE Arrow::arrow_shared OpenMP::OpenMP_CXX)
    # ICEBUG_ENABLED is read by gds_page_rank.cpp via #ifdef to skip its body when icebug is off
    # (defensive: source isn't compiled in that case, but matches the header-side guard).
    target_compile_definitions(lbug_algo_function PRIVATE ICEBUG_ENABLED)
else ()
    # Other sources still use <omp.h> for parallel regions, so keep the OpenMP link — just drop Arrow.
    target_link_libraries(lbug_algo_function PRIVATE OpenMP::OpenMP_CXX)
endif ()

if (WIN32)
    # This OBJECT library is compiled independently of the final lbug_algo_extension target.
    # lbug's INTERFACE LBUG_STATIC_DEFINE does not propagate here, but all sources in the
    # extension must use the empty LBUG_API (static-link ABI) on Windows to avoid LNK4217.
    target_compile_definitions(lbug_algo_function PRIVATE LBUG_STATIC_DEFINE)
endif()

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_algo_function>
        PARENT_SCOPE)
