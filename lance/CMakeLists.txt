cmake_minimum_required(VERSION 3.14...3.29)

# Set extension name here
set(TARGET_NAME lance)

set(EXTENSION_NAME ${TARGET_NAME}_extension)

project(${TARGET_NAME})
include_directories(src/include)

# Build and link the Rust staticlib (lance_ladybug_ffi), following the
# lance-duckdb CMake/Cargo integration: native Lance reads without DuckDB.
set(RUST_PLATFORM_TARGET "")

set(LANCE_PLATFORM "$ENV{LANCE_PLATFORM}")
if(LANCE_PLATFORM STREQUAL "linux_amd64_musl")
  set(RUST_PLATFORM_TARGET "x86_64-unknown-linux-musl")
elseif(LANCE_PLATFORM STREQUAL "linux_arm64_musl")
  set(RUST_PLATFORM_TARGET "aarch64-unknown-linux-musl")
elseif(LANCE_PLATFORM STREQUAL "windows_amd64_mingw")
  set(RUST_PLATFORM_TARGET "x86_64-pc-windows-gnu")
endif()

if(RUST_PLATFORM_TARGET STREQUAL "")
  if("${OS_NAME}" STREQUAL "linux")
    if("${OS_ARCH}" STREQUAL "arm64")
      set(RUST_PLATFORM_TARGET "aarch64-unknown-linux-gnu")
    else()
      set(RUST_PLATFORM_TARGET "x86_64-unknown-linux-gnu")
    endif()
  elseif("${OS_NAME}" STREQUAL "osx")
    if("${OSX_BUILD_ARCH}" STREQUAL "arm64")
      set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
    elseif("${OSX_BUILD_ARCH}" STREQUAL "x86_64")
      set(RUST_PLATFORM_TARGET "x86_64-apple-darwin")
    elseif("${OS_ARCH}" STREQUAL "arm64")
      set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
    else()
      set(RUST_PLATFORM_TARGET "x86_64-apple-darwin")
    endif()
  elseif("${OS_NAME}" STREQUAL "windows")
    if("${OS_ARCH}" STREQUAL "arm64")
      set(RUST_PLATFORM_TARGET "aarch64-pc-windows-msvc")
    else()
      set(RUST_PLATFORM_TARGET "x86_64-pc-windows-msvc")
    endif()
  endif()
endif()

if(RUST_PLATFORM_TARGET STREQUAL "")
  if(APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
    else()
      set(RUST_PLATFORM_TARGET "x86_64-apple-darwin")
    endif()
  elseif(UNIX)
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      set(RUST_PLATFORM_TARGET "aarch64-unknown-linux-gnu")
    else()
      set(RUST_PLATFORM_TARGET "x86_64-unknown-linux-gnu")
    endif()
  elseif(WIN32)
    set(RUST_PLATFORM_TARGET "x86_64-pc-windows-msvc")
  endif()
endif()

if(WIN32)
  if(RUST_PLATFORM_TARGET MATCHES "windows-gnu")
    set(RUST_STATICLIB_NAME "liblance_ladybug_ffi.a")
  else()
    set(RUST_STATICLIB_NAME "lance_ladybug_ffi.lib")
  endif()
else()
  set(RUST_STATICLIB_NAME "liblance_ladybug_ffi.a")
endif()

set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")
set(RUST_DEBUG_LIB
    "${RUST_TARGET_DIR}/${RUST_PLATFORM_TARGET}/debug/${RUST_STATICLIB_NAME}")
set(RUST_RELEASE_LIB
    "${RUST_TARGET_DIR}/${RUST_PLATFORM_TARGET}/release/${RUST_STATICLIB_NAME}")

# Cargo discovers Rust modules; CMake must notice changes to every module before
# deciding whether to invoke Cargo, including newly added FFI modules.
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/rust/src/*.rs")
set(RUST_FFI_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/rust/Cargo.toml
                     ${CMAKE_CURRENT_LIST_DIR}/rust/Cargo.lock ${RUST_SOURCES})

set(RUST_CARGO_ENV CARGO_TARGET_DIR=${RUST_TARGET_DIR}
                   MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
if(NOT "$ENV{PROTOC}" STREQUAL "")
  list(APPEND RUST_CARGO_ENV "PROTOC=$ENV{PROTOC}")
endif()

# Platform libs required by Rust deps (e.g., TLS, timezone).
if(APPLE)
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET
                                                STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET
        13.3
        CACHE STRING "Minimum OS X deployment version" FORCE)
  endif()
  set(PLATFORM_LIBS "-framework CoreFoundation" "-framework IOKit"
                    "-framework SystemConfiguration" "-framework Security")
elseif(UNIX)
  set(PLATFORM_LIBS m c)
elseif(WIN32)
  set(PLATFORM_LIBS
      ws2_32
      userenv
      advapi32
      crypt32
      ntdll
      bcrypt
      ncrypt
      secur32)
endif()

# Building both debug and release Rust artifacts is expensive and can trigger
# OOMs in CI. For single-config generators (Ninja/Makefiles), only build the
# active configuration's artifact.
if(CMAKE_CONFIGURATION_TYPES)
  add_custom_command(
    OUTPUT "${RUST_DEBUG_LIB}" "${RUST_RELEASE_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env ${RUST_CARGO_ENV} cargo build --manifest-path=${CMAKE_CURRENT_LIST_DIR}/rust/Cargo.toml --target=${RUST_PLATFORM_TARGET}
    COMMAND ${CMAKE_COMMAND} -E env ${RUST_CARGO_ENV} cargo build --manifest-path=${CMAKE_CURRENT_LIST_DIR}/rust/Cargo.toml --release --target=${RUST_PLATFORM_TARGET}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    DEPENDS ${RUST_FFI_DEPENDS}
    VERBATIM)
  add_custom_target(lance_ladybug_ffi_build DEPENDS "${RUST_DEBUG_LIB}" "${RUST_RELEASE_LIB}")
else()
  if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE Release)
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_command(
      OUTPUT "${RUST_DEBUG_LIB}"
      COMMAND ${CMAKE_COMMAND} -E env ${RUST_CARGO_ENV} cargo build --manifest-path=${CMAKE_CURRENT_LIST_DIR}/rust/Cargo.toml --target=${RUST_PLATFORM_TARGET}
      WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
      DEPENDS ${RUST_FFI_DEPENDS}
      VERBATIM)
    add_custom_target(lance_ladybug_ffi_build DEPENDS "${RUST_DEBUG_LIB}")
  else()
    add_custom_command(
      OUTPUT "${RUST_RELEASE_LIB}"
      COMMAND ${CMAKE_COMMAND} -E env ${RUST_CARGO_ENV} cargo build --manifest-path=${CMAKE_CURRENT_LIST_DIR}/rust/Cargo.toml --release --target=${RUST_PLATFORM_TARGET}
      WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
      DEPENDS ${RUST_FFI_DEPENDS}
      VERBATIM)
    add_custom_target(lance_ladybug_ffi_build DEPENDS "${RUST_RELEASE_LIB}")
  endif()
endif()

include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
)

add_subdirectory(src/function)
add_subdirectory(src/storage)
add_subdirectory(src/main)
add_subdirectory(test)

build_extension_lib(${BUILD_STATIC_EXTENSION} "lance")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        debug "${RUST_DEBUG_LIB}" optimized "${RUST_RELEASE_LIB}"
        ${PLATFORM_LIBS})
add_dependencies(lbug_${EXTENSION_LIB_NAME}_extension lance_ladybug_ffi_build)
