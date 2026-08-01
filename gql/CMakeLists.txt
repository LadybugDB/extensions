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
