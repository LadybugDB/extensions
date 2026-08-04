#pragma once

#include "function/function.h"

namespace lbug {
namespace timeseries_extension {

// ===== Function registration structs =====

struct EmbeddingSimilarityFunction {
    static constexpr const char* name = "EMBEDDING_SIMILARITY";
    static function::function_set getFunctionSet();
};

struct DetectDriftPointsFunction {
    static constexpr const char* name = "DETECT_DRIFT_POINTS";
    static function::function_set getFunctionSet();
};

} // namespace timeseries_extension
} // namespace lbug
