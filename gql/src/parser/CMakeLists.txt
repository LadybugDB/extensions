add_library(lbug_gql_extension_parser
        OBJECT
        GQLBaseVisitor.cpp
        GQLLexer.cpp
        GQLParser.cpp
        GQLVisitor.cpp)

# The generated ANTLR parser/lexer/visitor are warning-clean under -Wall but, like
# the parent repo's generated ANTLR code (see third_party/antlr4_cypher, which is
# silenced by -Wno-extra in the parent's third_party/CMakeLists.txt), they declare
# parameters that -Wextra flags as unused (e.g. the `_localctx` argument of the
# generated Sempred methods, which delegate to precpred(_ctx, ...)). Those become
# hard errors when the parent CI builds extensions with ENABLE_WERROR=1 / -Werror.
# Mirror the parent's treatment of generated code: disable the -Wextra-only warnings
# for this generated-object target only. Hand-written gql code is unaffected.
if(NOT MSVC)
    target_compile_options(lbug_gql_extension_parser PRIVATE -Wno-extra)
endif()

set(GQL_EXTENSION_OBJECT_FILES
        ${GQL_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_gql_extension_parser>
        PARENT_SCOPE)
