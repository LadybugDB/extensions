add_library(timeseries_extension_main
        OBJECT
        timeseries_extension.cpp
        )

set(TIMESERIES_EXTENSION_OBJECT_FILES
        ${TIMESERIES_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:timeseries_extension_main>
        PARENT_SCOPE)
