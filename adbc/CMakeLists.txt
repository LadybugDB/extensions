find_path(ADBC_INCLUDE_DIR
        NAMES arrow-adbc/adbc.h
        HINTS "$ENV{CONDA_PREFIX}/include")

find_library(ADBC_DRIVER_MANAGER_LIBRARY
        NAMES adbc_driver_manager
        HINTS "$ENV{CONDA_PREFIX}/lib")

if (NOT ADBC_INCLUDE_DIR OR NOT ADBC_DRIVER_MANAGER_LIBRARY)
    message(FATAL_ERROR "ADBC extension requires libadbc-driver-manager. Install the C++ ADBC dependencies with pixi/conda.")
endif ()

include_directories(
        src/include
        ${CMAKE_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/include
        ${ADBC_INCLUDE_DIR})

add_subdirectory(src/connector)
add_subdirectory(src/function)
add_subdirectory(src/catalog)
add_subdirectory(src/storage)
add_subdirectory(src/main)

build_extension_lib(${BUILD_STATIC_EXTENSION} "adbc")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        ${ADBC_DRIVER_MANAGER_LIBRARY})
