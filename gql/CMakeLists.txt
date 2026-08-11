include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/third_party/antlr4_runtime/src
        src/include
        src/parser
        ${PROJECT_SOURCE_DIR}/src/include/parser/antlr_parser)  # For GQL parser includes

add_subdirectory(src/parser)
add_subdirectory(src/function)
add_subdirectory(src/main)

build_extension_lib(${BUILD_STATIC_EXTENSION} "gql")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension PRIVATE antlr4_runtime)

# The gql extension statically links antlr4_runtime, which is *also* statically
# linked into the main liblbug.so (and thus exported from it). When this shared
# extension is dlopen'd, the two antlr4_runtime copies must not collide via the
# dynamic symbol table. Hide everything except the required `init`/`name`
# entry points so the extension's private antlr4_runtime stays private.
if (NOT WIN32 AND NOT BUILD_STATIC_EXTENSION)
    set_target_properties(lbug_${EXTENSION_LIB_NAME}_extension PROPERTIES
        LINK_FLAGS "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/gql_symver.map")
endif()
