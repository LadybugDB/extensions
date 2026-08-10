add_library(algo_extension_common
        OBJECT
        in_mem_graph.cpp
        in_mem_gds_utils.cpp
        )

if (WIN32)
    # This OBJECT library is compiled independently of the final lbug_algo_extension target.
    # lbug's INTERFACE LBUG_STATIC_DEFINE does not propagate here, but all sources in the
    # extension must use the empty LBUG_API (static-link ABI) on Windows to avoid LNK4217.
    target_compile_definitions(algo_extension_common PRIVATE LBUG_STATIC_DEFINE)
endif()

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:algo_extension_common>
        PARENT_SCOPE)
