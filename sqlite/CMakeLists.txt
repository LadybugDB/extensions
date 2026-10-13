# Native SQLite backend: the amalgamation is vendored so the extension
# builds without a system libsqlite3-dev (which also keeps Windows/macOS CI
# hermetic). It is public-domain (https://sqlite.org).
add_library(sqlite3_vendor STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3_vendor PUBLIC third_party/sqlite)
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(sqlite3_vendor PRIVATE -w)
endif ()
find_package(Threads REQUIRED)
target_link_libraries(sqlite3_vendor PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        third_party/sqlite)

add_subdirectory(src/connector)
add_subdirectory(src/catalog)
add_subdirectory(src/cdc)
add_subdirectory(src/storage)
add_subdirectory(src/main)
add_subdirectory(src/function)
add_subdirectory(test)

build_extension_lib(${BUILD_STATIC_EXTENSION} "sqlite")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        sqlite3_vendor)
