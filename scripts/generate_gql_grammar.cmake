# Regenerate the GQL ANTLR parser/lexer/visitor for the gql extension.
#
# The generated files live in gql/src/parser/ and are checked in.
#
# IMPORTANT: the ANTLR tool version must be 4.13.1 to match the vendored
# antlr4_runtime in the core repo (third_party/antlr4_runtime, Version.h ->
# 4.13.1). Regenerating with a newer tool (e.g. 4.13.2) produces a parse-tree
# class hierarchy the 4.13.1 runtime does not recognize: antlrcpp::downCast's
# dynamic_cast fails, which aborts in Debug builds and yields a type-confused
# pointer / SIGSEGV at exit in Release builds.
#
# Usage (from the extension repo root):
#   cmake -D ROOT_DIR=<abs path to extension repo> -P scripts/generate_gql_grammar.cmake

if(NOT DEFINED ROOT_DIR OR NOT EXISTS "${ROOT_DIR}/third_party/opengql/GQL.g4")
    message(FATAL_ERROR "ROOT_DIR must point at the extension repo containing third_party/opengql/GQL.g4")
endif()

set(ANTLR_JAR "${ROOT_DIR}/scripts/antlr-4.13.1-complete.jar")
set(ANTLR_SHA256 "bc13a9c57a8dd7d5196888211e5ede657cb64a3ce968608697e4f668251a8487")

if(NOT EXISTS "${ANTLR_JAR}")
    message(STATUS "Downloading antlr-4.13.1-complete.jar")
    file(DOWNLOAD https://www.antlr.org/download/antlr-4.13.1-complete.jar "${ANTLR_JAR}"
        EXPECTED_HASH SHA256=${ANTLR_SHA256})
endif()

find_package(Java REQUIRED)

set(OUT_DIR "${ROOT_DIR}/scripts/gql_grammar_generated")
file(MAKE_DIRECTORY "${OUT_DIR}")

# Generate from the extension repo root so the emitted "Generated from"
# comment reads third_party/opengql/GQL.g4. Flags match the checked-in files:
# visitor ON (-visitor), listener OFF (-no-listener), no package/namespace.
execute_process(
    COMMAND ${Java_JAVA_EXECUTABLE} -jar "${ANTLR_JAR}"
        -Dlanguage=Cpp -visitor -no-listener third_party/opengql/GQL.g4 -o "${OUT_DIR}"
    WORKING_DIRECTORY "${ROOT_DIR}"
    RESULT_VARIABLE gen_result)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "antlr generation failed: ${gen_result}")
endif()

set(PARSER_DIR "${ROOT_DIR}/gql/src/parser")
# antlr mirrors the grammar path (third_party/opengql/) under -o, so flatten:
file(GLOB_RECURSE GENERATED "${OUT_DIR}/*")
foreach(f ${GENERATED})
    if(NOT IS_DIRECTORY "${f}")
        get_filename_component(name "${f}" NAME)
        file(COPY "${f}" DESTINATION "${PARSER_DIR}/")
        message(STATUS "  updated ${PARSER_DIR}/${name}")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
message(STATUS "GQL grammar regenerated with ANTLR 4.13.1.")
