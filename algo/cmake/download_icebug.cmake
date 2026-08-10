# Resolve the icebug (NetworKit fork) library for the icebug-backed GDS_* algorithms.
# Sets: ICEBUG_INCLUDE_DIR (list of header dirs), ICEBUG_LIB (the library), ICEBUG_RPATH_DIR.
#
# Default: download the prebuilt release for this platform into vendor/ (Arrow + OpenMP stay
# system deps). Dev override: set -DICEBUG_SOURCE_DIR=<icebug checkout with a build/> to link a
# local source build instead (e.g. when the prebuilt's pinned Arrow version differs from yours).
#
# Reads the ICEBUG_ENABLED cache option from the parent CMakeLists.txt. If it's OFF we leave
# ICEBUG_INCLUDE_DIR / ICEBUG_LIB / ICEBUG_RPATH_DIR empty and return immediately. If the
# prebuilt download fails (offline CI, GitHub rate limit, etc.) we flip ICEBUG_ENABLED to OFF
# (cached, so re-configures don't re-fail), emit a WARNING, and return — the algo extension
# then builds without the GDS_* bridge instead of aborting configure.

if (NOT ICEBUG_ENABLED)
    message(STATUS "icebug: disabled via -DICEBUG_ENABLED=OFF; skipping download + lib resolution")
    set(ICEBUG_INCLUDE_DIR "")
    set(ICEBUG_LIB "ICEBUG_LIB-NOTFOUND")
    set(ICEBUG_RPATH_DIR "")
    return()
endif ()

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
    if (WIN32)
        # See the prebuilt branch below: on Windows the GlobalState singleton is a separate
        # DLL + import lib that the static networkit.lib imports via dllimport thunks.
        find_library(ICEBUG_STATE_LIB networkit_state PATHS "${ICEBUG_SOURCE_DIR}/build" NO_DEFAULT_PATH REQUIRED)
        set(ICEBUG_STATE_DLL "${ICEBUG_SOURCE_DIR}/build/networkit_state.dll")
    endif ()
    return()
endif ()

set(ICEBUG_VERSION "13.1" CACHE STRING "icebug release tag")
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
        message(WARNING
                "Failed to download icebug prebuilt (${_ib_url}): ${_ib_dl}\n"
                "  Disabling ICEBUG_ENABLED; the algo extension will still build with the "
                "hand-rolled algos (PAGE_RANK, SCC, etc.) but GDS_* functions won't be available.\n"
                "  To re-enable, ensure outbound HTTPS to github.com works, pre-populate "
                "extension/algo/vendor/ with the prebuilt, or pass "
                "-DICEBUG_SOURCE_DIR=<local checkout>.")
        set(ICEBUG_INCLUDE_DIR "")
        set(ICEBUG_LIB "ICEBUG_LIB-NOTFOUND")
        set(ICEBUG_RPATH_DIR "")
        # Force so re-configures don't retry the failing download.
        set(ICEBUG_ENABLED OFF CACHE BOOL "Enable icebug-backed GDS_* algorithms" FORCE)
        return()
    endif ()
    file(ARCHIVE_EXTRACT INPUT "${ICEBUG_VENDOR_DIR}/${_ib_asset}" DESTINATION "${ICEBUG_VENDOR_DIR}")
endif ()

set(ICEBUG_INCLUDE_DIR "${ICEBUG_VENDOR_DIR}/include")
set(ICEBUG_RPATH_DIR "${ICEBUG_VENDOR_DIR}/lib")
find_library(ICEBUG_LIB networkit PATHS "${ICEBUG_VENDOR_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
if (WIN32)
    # The vendored Windows artifact splits NetworKit into a large static lib (networkit.lib)
    # plus a separate DLL + import lib for the GlobalState singleton (networkit_state). The
    # static lib's members (Log.cpp.obj etc.) reference the GlobalState accessors via
    # __imp_* dllimport thunks, so consumers must ALSO link the import lib and deploy the DLL
    # next to the loaded extension.
    find_library(ICEBUG_STATE_LIB networkit_state PATHS "${ICEBUG_VENDOR_DIR}/lib/networkit" NO_DEFAULT_PATH REQUIRED)
    set(ICEBUG_STATE_DLL "${ICEBUG_VENDOR_DIR}/lib/networkit/networkit_state.dll")
    message(STATUS "icebug state lib: ${ICEBUG_STATE_LIB}")
endif ()
message(STATUS "icebug: ${ICEBUG_LIB}")
