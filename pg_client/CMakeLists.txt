find_package(PostgreSQL REQUIRED)

include_directories(
    ${PROJECT_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}/src/include
    src/include
    ${PostgreSQL_INCLUDE_DIRS})

add_subdirectory(src/connector)
add_subdirectory(src/storage)
add_subdirectory(src/main)
add_subdirectory(src/catalog)
add_subdirectory(src/function)

build_extension_lib(${BUILD_STATIC_EXTENSION} "pg_client")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
    PRIVATE
    ${PostgreSQL_LIBRARIES})
