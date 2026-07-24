# --- icebug (NetworKit fork) + Arrow, for the icebug-backed GDS_* algorithms ---
# Pulls the prebuilt icebug release into vendor/; Arrow + OpenMP are system deps.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/download_icebug.cmake)
# Help find_package locate Homebrew Arrow/OpenMP on macOS dev machines (no-op elsewhere).
if (APPLE AND EXISTS "/opt/homebrew/opt/apache-arrow")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/apache-arrow" "/opt/homebrew/opt/libomp")
endif ()
find_package(Arrow REQUIRED)
find_package(OpenMP REQUIRED)

include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        ${ICEBUG_INCLUDE_DIR})

add_subdirectory(src/main)
add_subdirectory(src/function)
add_subdirectory(src/common)
add_subdirectory(test)

build_extension_lib(${BUILD_STATIC_EXTENSION} "algo")

if (BUILD_STATIC_EXTENSION)
    set(_algo_target lbug_algo_static_extension)
else ()
    set(_algo_target lbug_algo_extension)
endif ()
target_link_libraries(${_algo_target} PRIVATE
        ${ICEBUG_LIB}
        Arrow::arrow_shared
        OpenMP::OpenMP_CXX)
# Let the loaded extension find a shared libnetworkit (prebuilt path); harmless for a static lib.
set_target_properties(${_algo_target} PROPERTIES
        BUILD_RPATH "${ICEBUG_RPATH_DIR}"
        INSTALL_RPATH "${ICEBUG_RPATH_DIR}")
