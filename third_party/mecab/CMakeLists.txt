# MeCab morphological analyzer — vendored from https://github.com/taku910/mecab
# (BSD-3-Clause, see LICENSE). Used by the FTS extension for Japanese
# tokenization. The ipadic dictionary (EUC-JP CSV) is compiled to the native
# binary dictionary format (UTF-8) at build time via mecab-dict-index.

set(MECAB_SRCS
        src/char_property.cpp
        src/connector.cpp
        src/context_id.cpp
        src/dictionary.cpp
        src/dictionary_compiler.cpp
        src/dictionary_generator.cpp
        src/dictionary_rewriter.cpp
        src/eval.cpp
        src/feature_index.cpp
        src/iconv_utils.cpp
        src/lbfgs.cpp
        src/learner.cpp
        src/learner_tagger.cpp
        src/libmecab.cpp
        src/nbest_generator.cpp
        src/param.cpp
        src/string_buffer.cpp
        src/tagger.cpp
        src/tokenizer.cpp
        src/utils.cpp
        src/viterbi.cpp
        src/writer.cpp)

# Same defines the upstream Makefile.msvc uses; VERSION/DIC_VERSION come from
# configure.in (0.996, 102). DLL_EXPORT is omitted: we link statically.
set(MECAB_DEFS
        _CRT_SECURE_NO_DEPRECATE
        MECAB_USE_THREAD
        HAVE_GETENV
        HAVE_WINDOWS_H
        DIC_VERSION=102
        VERSION=\"0.996\"
        PACKAGE=\"mecab\"
        # Default rc file: an empty file generated alongside the dictionary.
        # (An empty string would make Param::load() try to open "" and fail.)
        MECAB_DEFAULT_RC=\"${CMAKE_BINARY_DIR}/mecab-dict/ipadic/mecabrc\"
        # Gate for StringBuffer::operator<<(unsigned long long) in string_buffer.h.
        HAVE_UNSIGNED_LONG_LONG_INT)

add_library(mecab STATIC ${MECAB_SRCS})
target_include_directories(mecab PUBLIC src)
target_compile_definitions(mecab PRIVATE ${MECAB_DEFS})
# mecab.h declares MECAB_DLL_EXTERN as __declspec(dllimport) on _WIN32 unless
# DLL_EXPORT is defined. We link statically, so export (not import) must be
# declared consistently in both the library and its consumers.
target_compile_definitions(mecab PUBLIC DLL_EXPORT)
if(MSVC)
    # Upstream suppresses these in Makefile.msvc; the code is C++98-era.
    target_compile_options(mecab PRIVATE /wd4800 /wd4305 /wd4244 /wd4996)
endif()

# Build-time tool that compiles dictionary CSV files into the binary dictionary
# format (sys.dic/sys.dat/sys.cha/unk.*/char.bin/matrix.bin).
add_executable(mecab-dict-index src/mecab-dict-index.cpp ${MECAB_SRCS})
target_include_directories(mecab-dict-index PRIVATE src)
target_compile_definitions(mecab-dict-index PRIVATE ${MECAB_DEFS})
if(MSVC)
    target_compile_options(mecab-dict-index PRIVATE /wd4800 /wd4305 /wd4244 /wd4996)
endif()

# Compile the ipadic dictionary (EUC-JP CSV) into a UTF-8 binary dictionary.
# The dictionary is not vendored (54MB CSV): it is downloaded from
# https://github.com/taku910/mecab-ipadic at configure time. The compiled
# dictionary files are host-endian, so they are generated at build time.
# ipadic has no standalone repo; it lives in taku910/mecab's mecab-ipadic/
# subdirectory, so we download the mecab archive and keep that subtree.
set(MECAB_IPADIC_DIR ${CMAKE_BINARY_DIR}/mecab-ipadic-src)
if(NOT EXISTS ${MECAB_IPADIC_DIR}/dicrc)
    set(MECAB_IPADIC_ZIP ${CMAKE_BINARY_DIR}/mecab-ipadic.zip)
    set(MECAB_IPADIC_EXTRACT ${CMAKE_BINARY_DIR}/mecab-ipadic-extract)
    message(STATUS "Downloading mecab-ipadic dictionary sources (54MB CSV)...")
    file(DOWNLOAD https://github.com/taku910/mecab/archive/refs/heads/master.zip
            ${MECAB_IPADIC_ZIP} STATUS MECAB_DL_STATUS)
    list(GET MECAB_DL_STATUS 0 MECAB_DL_CODE)
    if(NOT MECAB_DL_CODE EQUAL 0)
        list(GET MECAB_DL_STATUS 1 MECAB_DL_ERROR)
        message(FATAL_ERROR "Failed to download mecab-ipadic: ${MECAB_DL_ERROR}")
    endif()
    file(MAKE_DIRECTORY ${MECAB_IPADIC_EXTRACT})
    file(ARCHIVE_EXTRACT INPUT ${MECAB_IPADIC_ZIP} DESTINATION ${MECAB_IPADIC_EXTRACT})
    set(MECAB_IPADIC_SUBDIR ${MECAB_IPADIC_EXTRACT}/mecab-master/mecab-ipadic)
    if(NOT IS_DIRECTORY "${MECAB_IPADIC_SUBDIR}")
        message(FATAL_ERROR "Unexpected mecab archive layout (mecab-ipadic dir missing)")
    endif()
    file(RENAME ${MECAB_IPADIC_SUBDIR} ${MECAB_IPADIC_DIR})
    file(REMOVE_RECURSE ${MECAB_IPADIC_EXTRACT})
    file(REMOVE ${MECAB_IPADIC_ZIP})
endif()
set(MECAB_DICT_OUT_DIR ${CMAKE_BINARY_DIR}/mecab-dict/ipadic)
set(MECAB_DICT_OUTPUTS
        ${MECAB_DICT_OUT_DIR}/sys.dic
        ${MECAB_DICT_OUT_DIR}/unk.dic
        ${MECAB_DICT_OUT_DIR}/char.bin
        ${MECAB_DICT_OUT_DIR}/matrix.bin
        ${MECAB_DICT_OUT_DIR}/dicrc
        ${MECAB_DICT_OUT_DIR}/mecabrc)

add_custom_command(
        OUTPUT ${MECAB_DICT_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MECAB_DICT_OUT_DIR}
        COMMAND $<TARGET_FILE:mecab-dict-index> -f euc-jp -t utf-8
                -d ${MECAB_IPADIC_DIR} -o ${MECAB_DICT_OUT_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${MECAB_IPADIC_DIR}/dicrc ${MECAB_DICT_OUT_DIR}/dicrc
        COMMAND ${CMAKE_COMMAND} -E touch ${MECAB_DICT_OUT_DIR}/mecabrc
        DEPENDS mecab-dict-index
        COMMENT "Compiling ipadic dictionary (EUC-JP -> UTF-8)")

add_custom_target(mecab-ipadic-dict
        DEPENDS ${MECAB_DICT_OUT_DIR}/sys.dic)

# CLI for ad-hoc tokenization checks during development.
add_executable(mecab-cli src/mecab.cpp ${MECAB_SRCS})
target_include_directories(mecab-cli PRIVATE src)
target_compile_definitions(mecab-cli PRIVATE ${MECAB_DEFS})
if(MSVC)
    target_compile_options(mecab-cli PRIVATE /wd4800 /wd4305 /wd4244 /wd4996)
endif()
