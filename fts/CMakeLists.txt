include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        third_party/snowball/libstemmer
        ${PROJECT_SOURCE_DIR}/third_party/cppjieba/include
        ${PROJECT_SOURCE_DIR}/third_party/cppjieba/deps/limonp/include
        third_party/mecab/src)

add_subdirectory(src/catalog)
add_subdirectory(src/function)
add_subdirectory(src/main)
add_subdirectory(test)
add_subdirectory(src/index)
add_subdirectory(src/utils)

add_subdirectory(third_party/snowball)

# MeCab (Japanese tokenizer), vendored in-tree like snowball: static lib +
# mecab-dict-index tool + ipadic dictionary compilation at build time
# (downloads the dictionary CSV at configure time when no prebuilt dictionary
# is bundled).
add_subdirectory(third_party/mecab)

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
