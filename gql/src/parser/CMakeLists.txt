add_library(lbug_gql_extension_parser
        OBJECT
        GQLBaseVisitor.cpp
        GQLLexer.cpp
        GQLParser.cpp
        GQLVisitor.cpp)

set(GQL_EXTENSION_OBJECT_FILES
        ${GQL_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_gql_extension_parser>
        PARENT_SCOPE)
