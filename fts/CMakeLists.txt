# Vendored dependencies live in the extensions repo root's third_party/.
# Use CMAKE_CURRENT_LIST_DIR (not PROJECT_SOURCE_DIR) so this also resolves
# when built under the engine's build system, where PROJECT_SOURCE_DIR points
# at the engine checkout.
set(FTS_EXTENSIONS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        third_party/snowball/libstemmer
        ${FTS_EXTENSIONS_ROOT}/third_party/cppjieba/include
        ${FTS_EXTENSIONS_ROOT}/third_party/cppjieba/deps/limonp/include
        ${FTS_EXTENSIONS_ROOT}/third_party/mecab/src)

# cppjieba (jieba tokenizer dependency) is a header-only interface library;
# upstream referenced it but never vendored it, so define the target here.
if(NOT TARGET cppjieba)
    add_library(cppjieba INTERFACE)
    target_include_directories(cppjieba INTERFACE
            ${FTS_EXTENSIONS_ROOT}/third_party/cppjieba/include
            ${FTS_EXTENSIONS_ROOT}/third_party/cppjieba/deps/limonp/include)
endif()

# MeCab (Japanese tokenizer): static lib + mecab-dict-index tool + build-time
# ipadic dictionary compilation (downloads the dictionary CSV at configure).
# The source dir is outside this extension's tree, so an explicit binary dir
# is required (this also applies when built under the engine's build system).
add_subdirectory(${FTS_EXTENSIONS_ROOT}/third_party/mecab
        ${CMAKE_CURRENT_BINARY_DIR}/mecab)

add_subdirectory(src/catalog)
add_subdirectory(src/function)
add_subdirectory(src/main)
add_subdirectory(test)
add_subdirectory(src/index)
add_subdirectory(src/utils)

add_subdirectory(third_party/snowball)

# tokenizer.cpp includes mecab.h, which requires DLL_EXPORT to declare the
# MeCab API without __declspec(dllimport) on Windows.
target_compile_definitions(lbug_fts_utils PRIVATE DLL_EXPORT)

build_extension_lib(${BUILD_STATIC_EXTENSION} "fts")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        snowball
        re2
        cppjieba
        mecab)

# Copy Jieba dictionaries next to the built extension so runtime can find a default path
add_custom_command(TARGET lbug_${EXTENSION_LIB_NAME}_extension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/third_party/cppjieba/dict
        ${PROJECT_SOURCE_DIR}/extension/fts/build/dict)

# Copy the compiled ipadic dictionary (UTF-8) next to the built extension so
# the default mecab_dict_dir resolves at runtime.
add_custom_command(TARGET lbug_${EXTENSION_LIB_NAME}_extension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/mecab-dict/ipadic
        ${PROJECT_SOURCE_DIR}/extension/fts/build/dict-ipadic)
add_dependencies(lbug_${EXTENSION_LIB_NAME}_extension mecab-ipadic-dict)
