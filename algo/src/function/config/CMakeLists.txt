add_library(lbug_algo_config
        OBJECT
        max_iterations_config.cpp)

if (WIN32)
    # This OBJECT library is compiled independently of the final lbug_algo_extension target.
    # lbug's INTERFACE LBUG_STATIC_DEFINE does not propagate here, but all sources in the
    # extension must use the empty LBUG_API (static-link ABI) on Windows to avoid LNK4217.
    target_compile_definitions(lbug_algo_config PRIVATE LBUG_STATIC_DEFINE)
endif()

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_algo_config>
        PARENT_SCOPE)
