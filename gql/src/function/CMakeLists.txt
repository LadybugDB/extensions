add_library(lbug_gql_extension_function
        OBJECT
        gql_function.cpp)

set(GQL_EXTENSION_OBJECT_FILES
        ${GQL_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_gql_extension_function>
        PARENT_SCOPE)
