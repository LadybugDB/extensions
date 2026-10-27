add_library(lbug_lance_storage
        OBJECT
        lance_reader.cpp
        lance_node_table.cpp
        lance_rel_table.cpp)

set(LANCE_EXTENSION_OBJECT_FILES
        ${LANCE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_lance_storage>
        PARENT_SCOPE)
