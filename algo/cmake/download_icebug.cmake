# Resolve the icebug (NetworKit fork) library for the icebug-backed GDS_* algorithms.
# Sets: ICEBUG_INCLUDE_DIR (list of header dirs), ICEBUG_LIB (the library), ICEBUG_RPATH_DIR.
#
# Default: download the prebuilt release for this platform into vendor/ (Arrow + OpenMP stay
# system deps). Dev override: set -DICEBUG_SOURCE_DIR=<icebug checkout with a build/> to link a
# local source build instead (e.g. when the prebuilt's pinned Arrow version differs from yours).

if (NOT DEFINED ICEBUG_SOURCE_DIR AND DEFINED ENV{ICEBUG_SOURCE_DIR})
    set(ICEBUG_SOURCE_DIR "$ENV{ICEBUG_SOURCE_DIR}")
endif ()

if (DEFINED ICEBUG_SOURCE_DIR)
    message(STATUS "icebug: using local source build at ${ICEBUG_SOURCE_DIR}")
    set(ICEBUG_INCLUDE_DIR
            "${ICEBUG_SOURCE_DIR}/include"
            "${ICEBUG_SOURCE_DIR}/extlibs/tlx"
            "${ICEBUG_SOURCE_DIR}/extlibs/ttmath")
    find_library(ICEBUG_LIB networkit PATHS "${ICEBUG_SOURCE_DIR}/build" NO_DEFAULT_PATH REQUIRED)
    get_filename_component(ICEBUG_RPATH_DIR "${ICEBUG_LIB}" DIRECTORY)
    return()
endif ()

set(ICEBUG_VERSION "12.8" CACHE STRING "icebug release tag")
set(ICEBUG_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

if (APPLE)
    set(_ib_os "macos")
elseif (WIN32)
    set(_ib_os "win")
else ()
    set(_ib_os "linux")
endif ()
if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(_ib_arch "arm64")
elseif (WIN32)
    set(_ib_arch "amd64")
else ()
    set(_ib_arch "x86_64")
endif ()
if (WIN32)
    set(_ib_asset "icebug-${_ib_os}-${_ib_arch}.zip")
else ()
    set(_ib_asset "icebug-${_ib_os}-${_ib_arch}.tar.gz")
endif ()

if (NOT EXISTS "${ICEBUG_VENDOR_DIR}/lib")
    set(_ib_url
            "https://github.com/Ladybug-Memory/icebug/releases/download/${ICEBUG_VERSION}/${_ib_asset}")
    message(STATUS "Downloading icebug prebuilt: ${_ib_url}")
    file(MAKE_DIRECTORY "${ICEBUG_VENDOR_DIR}")
    file(DOWNLOAD "${_ib_url}" "${ICEBUG_VENDOR_DIR}/${_ib_asset}" STATUS _ib_dl SHOW_PROGRESS)
    list(GET _ib_dl 0 _ib_dl_code)
    if (NOT _ib_dl_code EQUAL 0)
        message(FATAL_ERROR "Failed to download icebug (${_ib_url}): ${_ib_dl}")
    endif ()
    file(ARCHIVE_EXTRACT INPUT "${ICEBUG_VENDOR_DIR}/${_ib_asset}" DESTINATION "${ICEBUG_VENDOR_DIR}")
endif ()

set(ICEBUG_INCLUDE_DIR "${ICEBUG_VENDOR_DIR}/include")
set(ICEBUG_RPATH_DIR "${ICEBUG_VENDOR_DIR}/lib")
find_library(ICEBUG_LIB networkit PATHS "${ICEBUG_VENDOR_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
message(STATUS "icebug: ${ICEBUG_LIB}")
