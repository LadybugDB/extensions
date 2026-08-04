add_library(lbug_timeseries_function
        OBJECT
        embedding_similarity.cpp
        detect_drift_points.cpp
        )

set(TIMESERIES_EXTENSION_OBJECT_FILES
        ${TIMESERIES_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_timeseries_function>
        PARENT_SCOPE)
