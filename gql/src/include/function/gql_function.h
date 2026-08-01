#pragma once

#include "function/function.h"

namespace lbug {
namespace gql_extension {

struct GqlFunction {
    static constexpr const char* name = "GQL";

    static function::function_set getFunctionSet();
};

} // namespace gql_extension
} // namespace lbug
