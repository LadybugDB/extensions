add_library(lbug_gql_extension_transformer
        OBJECT
        ${CMAKE_CURRENT_SOURCE_DIR}/../gql_transformer.cpp)

add_library(lbug_gql_extension_main
        OBJECT
        gql_extension.cpp)

set(GQL_EXTENSION_OBJECT_FILES
        ${GQL_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_gql_extension_transformer>
        $<TARGET_OBJECTS:lbug_gql_extension_main>
        PARENT_SCOPE)
