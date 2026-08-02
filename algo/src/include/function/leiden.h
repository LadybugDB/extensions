#pragma once

#include "function/function.h"

namespace lbug {
namespace algo_extension {

struct LeidenFunction {
    static constexpr const char* name = "LEIDEN";

    static function::function_set getFunctionSet();
};

struct LeidenAliasFunction {
    using alias = LeidenFunction;

    static constexpr const char* name = "LE";
};

} // namespace algo_extension
} // namespace lbug
