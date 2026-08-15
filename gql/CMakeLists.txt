include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/third_party/antlr4_runtime/src
        src/include
        src/parser
        ${PROJECT_SOURCE_DIR}/src/include/parser/antlr_parser)  # For GQL parser includes

# antlr4_runtime is built as a static library (see third_party/antlr4_runtime/CMakeLists.txt).
# On Windows its headers default to `__declspec(dllimport)`, which makes MSVC emit
# references to the (non-existent) ANTLR DLL for inline/compiler-generated members
# such as AbstractParseTreeVisitor::shouldVisitNextChild and its destructor, causing
# LNK2001. Define ANTLR4CPP_STATIC so those members are emitted locally, matching how
# the core library builds its own ANTLR consumers (src/CMakeLists.txt).
add_compile_definitions(ANTLR4CPP_STATIC)

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
    if (APPLE)
        # Apple's ld64 does not support GNU ld's --version-script; the
        # equivalent is -exported_symbols_list. Preserve the pre-existing
        # -undefined dynamic_lookup flag that build_extension_lib() set.
        get_target_property(_gql_link_flags lbug_${EXTENSION_LIB_NAME}_extension LINK_FLAGS)
        set_target_properties(lbug_${EXTENSION_LIB_NAME}_extension PROPERTIES
            LINK_FLAGS "${_gql_link_flags} -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/gql_exported_symbols.list")
    else ()
        set_target_properties(lbug_${EXTENSION_LIB_NAME}_extension PROPERTIES
            LINK_FLAGS "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/gql_symver.map")
    endif ()
endif()
