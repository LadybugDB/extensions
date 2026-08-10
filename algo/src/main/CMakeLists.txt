add_library(algo_extension_main
        OBJECT
        algo_extension.cpp
        )

if (ICEBUG_ENABLED)
    # ICEBUG_ENABLED is read by algo_extension.cpp via #ifdef to gate the GDS_PAGE_RANK
    # registration and the ARROW_DEFAULT_MEMORY_POOL setup.
    target_compile_definitions(algo_extension_main PRIVATE ICEBUG_ENABLED)
endif ()

if (WIN32)
    # This OBJECT library is compiled independently of the final lbug_algo_extension target.
    # lbug's INTERFACE LBUG_STATIC_DEFINE does not propagate here, but all sources in the
    # extension must use the empty LBUG_API (static-link ABI) on Windows to avoid LNK4217.
    target_compile_definitions(algo_extension_main PRIVATE LBUG_STATIC_DEFINE)
endif()

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:algo_extension_main>
        PARENT_SCOPE)
