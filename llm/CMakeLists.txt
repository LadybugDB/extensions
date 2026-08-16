SET(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
SET(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)
# OpenSSL is resolved by the top-level build before extensions are configured.
# LBUG_REQUIRE_STATIC_OPENSSL must therefore be set at the top level.
find_package(OpenSSL REQUIRED)
# Official dynamic extension packages must set this option so their LLM artifact
# does not acquire a host libcurl dependency. Keep developer builds opt-in: a
# static curl is not installed by every development environment.
option(LLM_REQUIRE_STATIC_CURL "Require static libcurl for the LLM extension" OFF)
if (LLM_REQUIRE_STATIC_CURL)
    set(CURL_USE_STATIC_LIBS TRUE)
    message(STATUS "LLM curl linkage: static library required")
else ()
    message(STATUS "LLM curl linkage: toolchain default")
endif ()
find_package(CURL REQUIRED)
message(STATUS "LLM curl library: ${CURL_LIBRARY}")
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/extension/httpfs/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        ${PROJECT_SOURCE_DIR}/third_party/simsimd/include
        ${OPENSSL_INCLUDE_DIR}
        ${CURL_INCLUDE_DIRS})

#Used by the AmazonBedrock provider for signing
add_library(httpfs_crypto OBJECT ${PROJECT_SOURCE_DIR}/extension/httpfs/src/crypto.cpp)

add_subdirectory(src/function)
add_subdirectory(src/main)
add_subdirectory(src/providers)

build_extension_lib(${BUILD_STATIC_EXTENSION} "llm")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        ${OPENSSL_LIBRARIES}
        mbedtls
        $<TARGET_OBJECTS:httpfs_crypto>
)

if (TARGET CURL::libcurl)
    target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension PRIVATE CURL::libcurl)
else ()
    target_include_directories(lbug_${EXTENSION_LIB_NAME}_extension PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension PRIVATE ${CURL_LIBRARIES})
endif ()
