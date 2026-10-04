#pragma once

#include "common/vector/value_vector.h"
#include "function/function.h"

namespace lbug {
namespace gql_extension {

// GQL extension functions backing mixed-type comparison and GQL aggregation:
//   _GQL_TO_JSON(ANY) -> JSON          encode any value as JSON text
//   _GQL_MAX(ANY) -> <argument type>   max over values (bindFunc pins the
//   _GQL_MIN(ANY) -> <argument type>   result type to the input's logical type)
//   _GQL_SUM(ANY) -> INT128/UINT128/DOUBLE/JSON   sum over numeric or JSON
//   _GQL_AVG(ANY) -> DOUBLE/JSON                  values (bindFunc pins the
//                                                  result type likewise)
//   _GQL_LT/_GQL_LE/_GQL_GT/_GQL_GE(ANY, ANY) -> BOOL
//                                      GQL total-order comparisons; operands
//                                      are classified per value from their own
//                                      logical type (no bindFunc pins them)
//   _GQL_SORTKEY(ANY) -> STRING        byte-comparable encoding of the same
//                                      total order for ORDER BY
// The `_GQL_` prefix keeps them clear of the JSON extension's `to_json` (the
// catalog is case-insensitive; `extension::addFunc` is idempotent).

struct GqlToJsonFunction {
    static constexpr const char* name = "_GQL_TO_JSON";

    static function::function_set getFunctionSet();

    static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
        const std::vector<common::SelectionVector*>& parameterSelVectors,
        common::ValueVector& result, common::SelectionVector* resultSelVector,
        void* /*dataPtr*/);
};

struct GqlMaxFunction {
    static constexpr const char* name = "_GQL_MAX";

    static function::function_set getFunctionSet();
};

struct GqlMinFunction {
    static constexpr const char* name = "_GQL_MIN";

    static function::function_set getFunctionSet();
};

struct GqlSumFunction {
    static constexpr const char* name = "_GQL_SUM";

    static function::function_set getFunctionSet();
};

struct GqlAvgFunction {
    static constexpr const char* name = "_GQL_AVG";

    static function::function_set getFunctionSet();
};

struct GqlLtFunction {
    static constexpr const char* name = "_GQL_LT";

    static function::function_set getFunctionSet();
};

struct GqlLeFunction {
    static constexpr const char* name = "_GQL_LE";

    static function::function_set getFunctionSet();
};

struct GqlGtFunction {
    static constexpr const char* name = "_GQL_GT";

    static function::function_set getFunctionSet();
};

struct GqlGeFunction {
    static constexpr const char* name = "_GQL_GE";

    static function::function_set getFunctionSet();
};

struct GqlEqFunction {
    static constexpr const char* name = "_GQL_EQ";

    static function::function_set getFunctionSet();
};

struct GqlNeFunction {
    static constexpr const char* name = "_GQL_NE";

    static function::function_set getFunctionSet();
};

struct GqlSortKeyFunction {
    static constexpr const char* name = "_GQL_SORTKEY";

    static function::function_set getFunctionSet();
};

// _GQL_IS_SIMPLE(ANY) -> BOOL — ISO GQL SIMPLE path predicate: every node of
// the path distinct except that the first and last may be the same (closed
// cycle). The engine has no such mode (`*ACYCLIC` only distincts intermediate
// nodes and IS_ACYCLIC forbids first=last), so the GQL SIMPLE path mode wraps
// its path variable in this predicate.
struct GqlIsSimpleFunction {
    static constexpr const char* name = "_GQL_IS_SIMPLE";

    static function::function_set getFunctionSet();

    static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
        const std::vector<common::SelectionVector*>& parameterSelVectors,
        common::ValueVector& result, common::SelectionVector* resultSelVector,
        void* /*dataPtr*/);
};

// _GQL_LIST_CHECKED(ANY...) -> LIST — Q5-3 bind-time guard against the
// engine's list-binding homogenization on typed graphs. The translation layer
// wraps every list literal that contains at least one non-literal element in
// this call; bindFunc compares the engine-derived argument type classes (the
// scanValueShapes int/double/string/bool/list/map partition) and throws
// "GQL feature not supported: heterogeneous list element types" on a
// mismatch, where the engine's own list_creation would silently sink the
// losers to STRING. Execution assembles the N argument vectors into one list
// exactly like ListCreationFunction.
struct GqlListCheckedFunction {
    static constexpr const char* name = "_GQL_LIST_CHECKED";

    static function::function_set getFunctionSet();

    static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
        const std::vector<common::SelectionVector*>& parameterSelVectors,
        common::ValueVector& result, common::SelectionVector* resultSelVector,
        void* /*dataPtr*/);
};

} // namespace gql_extension
} // namespace lbug
