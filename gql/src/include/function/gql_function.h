#pragma once

#include "common/vector/value_vector.h"
#include "function/function.h"

namespace lbug {
namespace gql_extension {

struct GqlFunction {
    static constexpr const char* name = "GQL";

    static function::function_set getFunctionSet();
};

// _gql_schemas() -> STRING: scalar function exposing the schema catalog as
// JSON text — {"schemas":[...],"directories":[...]} with both arrays sorted
// lexicographically. Consumed by the TCK harness to verify CREATE/DROP
// SCHEMA side effects against its own catalog model.
struct GqlSchemasFunction {
    static constexpr const char* name = "_gql_schemas";

    static function::function_set getFunctionSet();

    static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
        const std::vector<common::SelectionVector*>& parameterSelVectors,
        common::ValueVector& result, common::SelectionVector* resultSelVector,
        void* dataPtr);
};

} // namespace gql_extension
} // namespace lbug
