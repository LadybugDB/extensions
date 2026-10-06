#include "function/gql_json_functions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#include "binder/expression/expression_util.h"
#include "common/assert.h"
#include "common/constants.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/json_utils.h"
#include "common/type_utils.h"
#include "common/types/json_type.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "function/aggregate_function.h"
#include "function/arithmetic/add.h"
#include "function/comparison/comparison_functions.h"
#include "function/hash/hash_functions.h"
#include "function/scalar_function.h"
#include "yyjson.h"

namespace lbug {
namespace gql_extension {

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::json_extension;

// =============================================================================
// _GQL_TO_JSON(ANY) -> JSON
// =============================================================================
// Mirrors extension/json's to_json: INT64 encodes as a JSON integer (`5`),
// DOUBLE as a JSON real (`5.0`; yyjson appends `.0` to integer-valued reals),
// STRING as a quoted JSON string, lists as compact JSON arrays, and a NULL
// input yields a NULL result (never the text `null`). JSON-typed input passes
// through byte-for-byte: it is already JSON text, and re-serializing it would
// rewrite formatting the caller may rely on.

namespace {

// JSON has no spelling for NaN/±Inf, and the serializer would silently forge
// one (`null` or invalid text) — exactly the wrong-value class this layer
// forbids. The order siblings reject non-finite via realToNumberText; this
// walks the value (including list/map/struct nesting) and rejects before
// jsonifying. JSON-typed input is skipped by the caller: it is already text.
void ensureFiniteForJson(const ValueVector& vec, uint64_t pos) {
    switch (vec.dataType.getLogicalTypeID()) {
    case LogicalTypeID::DOUBLE: {
        auto val = vec.getValue<double>(pos);
        if (!std::isfinite(val)) {
            throw RuntimeException(std::format(
                "_GQL_TO_JSON: non-finite value cannot be serialized as JSON: {}", val));
        }
        break;
    }
    case LogicalTypeID::FLOAT: {
        auto val = static_cast<double>(vec.getValue<float>(pos));
        if (!std::isfinite(val)) {
            throw RuntimeException(std::format(
                "_GQL_TO_JSON: non-finite value cannot be serialized as JSON: {}", val));
        }
        break;
    }
    case LogicalTypeID::LIST:
    case LogicalTypeID::ARRAY:
    case LogicalTypeID::MAP: {
        auto& entry = vec.getValue<list_entry_t>(pos);
        auto* dataVector = ListVector::getDataVector(&vec);
        for (auto i = 0u; i < entry.size; ++i) {
            ensureFiniteForJson(*dataVector, entry.offset + i);
        }
        break;
    }
    case LogicalTypeID::STRUCT: {
        auto fieldCount = StructType::getNumFields(vec.dataType);
        for (auto i = 0u; i < fieldCount; ++i) {
            ensureFiniteForJson(*StructVector::getFieldVector(&vec, i), pos);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

void GqlToJsonFunction::execFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    DASSERT(parameters.size() == 1);
    result.resetAuxiliaryBuffer();
    for (auto i = 0u; i < resultSelVector->getSelSize(); ++i) {
        auto inputPos = (*parameterSelVectors[0])[i];
        auto resultPos = (*resultSelVector)[i];
        auto isNull = parameters[0]->isNull(inputPos);
        result.setNull(resultPos, isNull);
        if (isNull) {
            continue;
        }
        if (JsonType::isJson(parameters[0]->dataType)) {
            auto jsonVal = parameters[0]->getValue<string_t>(inputPos);
            StringVector::addString(&result, resultPos, jsonVal);
        } else {
            ensureFiniteForJson(*parameters[0], inputPos);
            StringVector::addString(&result, resultPos,
                jsonToString(jsonify(*parameters[0], inputPos)));
        }
    }
}

static std::unique_ptr<FunctionBindData> bindToJson(const ScalarBindFuncInput& input) {
    DASSERT(input.arguments.size() == 1);
    LogicalType type = input.arguments[0]->dataType.copy();
    if (type.getLogicalTypeID() == LogicalTypeID::ANY) {
        type = LogicalType::INT64();
    }
    auto bindData = std::make_unique<FunctionBindData>(JsonType::getJsonType());
    bindData->paramTypes.push_back(std::move(type));
    return bindData;
}

function_set GqlToJsonFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::JSON, execFunc);
    func->bindFunc = bindToJson;
    result.push_back(std::move(func));
    return result;
}

// =============================================================================
// _GQL_MAX / _GQL_MIN(ANY) -> <argument type>
// =============================================================================

namespace {

// Aggregate state for _GQL_MAX/_GQL_MIN.
//
// Contract (see AggregateState in function/aggregate_function.h): states are
// memcpy'd into factorized tables and their destructors never run, so every
// member must be trivially destructible. The winning value lives in `raw` as
// its physical representation; STRING/BLOB/JSON values store a string_t
// header whose overflow bytes are copied into the aggregate's
// InMemOverflowBuffer exactly like MinMaxFunction<string_t>::setVal
// (src/include/function/aggregate/min_max.h).
//
// `storedType` is the bound argument's logical type: one query binds one
// type, so it is constant for the lifetime of a state. ANY only ever appears
// while `isNull` is still true (no value stored yet).
struct GqlMinMaxState : public AggregateStateWithNull {
    // Largest physical representation: string_t / int128_t / uint128_t.
    static constexpr uint32_t MAX_VALUE_SIZE = 16;

    LogicalTypeID storedType = LogicalTypeID::ANY;
    alignas(8) uint8_t raw[MAX_VALUE_SIZE] = {};

    uint32_t getStateSize() const override { return sizeof(*this); }
    void writeToVector(ValueVector* outputVector, uint64_t pos) override;

    template<typename T>
    T load() const {
        T val;
        memcpy(&val, raw, sizeof(T));
        return val;
    }

    // Copies `val_` into the state, allocating overflow space for long strings
    // (same reuse policy as MinMaxFunction<string_t>::setVal: only grow when
    // the new value is a long string longer than the stored one).
    template<typename T>
    void setVal(const T& val_, InMemOverflowBuffer* overflowBuffer) {
        if constexpr (std::is_same_v<T, string_t>) {
            string_t dst;
            memcpy(&dst, raw, sizeof(string_t));
            if (val_.len > string_t::SHORT_STR_LENGTH && val_.len > dst.len) {
                dst.overflowPtr =
                    reinterpret_cast<uint64_t>(overflowBuffer->allocateSpace(val_.len));
            }
            dst.set(val_);
            memcpy(raw, &dst, sizeof(string_t));
        } else {
            memcpy(raw, &val_, sizeof(T));
        }
    }
};

// The type families built-in MIN/MAX instantiate (min_max.cpp loops over
// LogicalTypeUtils::getAllValidComparableLogicalTypes()), plus JSON. Anything
// else — LIST, STRUCT, MAP, NODE, ANY, ... — is rejected loudly rather than
// silently mis-ordered. Function-local static: built once, cheap to scan.
bool isSupportedInput(const LogicalType& type) {
    if (type.getLogicalTypeID() == LogicalTypeID::JSON) {
        return true;
    }
    static const std::vector<LogicalTypeID> supportedTypes =
        LogicalTypeUtils::getAllValidComparableLogicalTypes();
    return std::find(supportedTypes.begin(), supportedTypes.end(), type.getLogicalTypeID()) !=
           supportedTypes.end();
}

std::string unsupportedTypeError(const LogicalType& type) {
    return std::format("_GQL_MAX/_GQL_MIN: unsupported input type {}",
        LogicalTypeUtils::toString(type.getLogicalTypeID()));
}

void GqlMinMaxState::writeToVector(ValueVector* outputVector, uint64_t pos) {
    if (storedType == LogicalTypeID::ANY) {
        throw RuntimeException("_GQL_MAX/_GQL_MIN: no value to materialize");
    }
    // JSON visits as string_t (PhysicalTypeID::JSON), so JSON results are
    // written like STRING results: raw JSON text into a JSON-typed vector.
    TypeUtils::visit(LogicalType::getPhysicalType(storedType),
        [&]<ComparableTypes T>(T) { outputVector->setValue<T>(pos, load<T>()); },
        [&](auto) -> void { throw RuntimeException(unsupportedTypeError(LogicalType(storedType))); });
}

void storeFromVector(GqlMinMaxState* state, ValueVector* input, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    state->storedType = input->dataType.getLogicalTypeID();
    TypeUtils::visit(input->dataType.getPhysicalType(),
        [&]<ComparableTypes T>(T) {
            state->template setVal<T>(input->getValue<T>(pos), overflowBuffer);
        },
        [&](auto) -> void {
            throw RuntimeException(unsupportedTypeError(input->dataType));
        });
}

// Merges `other` into `state`. The incoming string_t header may point into
// `other`'s overflow buffer, so the bytes are re-copied through setVal into
// the combine-provided buffer — the same re-copy MinMaxFunction::combine does.
void storeFromState(GqlMinMaxState* state, const GqlMinMaxState* other,
    InMemOverflowBuffer* overflowBuffer) {
    state->storedType = other->storedType;
    TypeUtils::visit(LogicalType::getPhysicalType(other->storedType),
        [&]<ComparableTypes T>(T) {
            state->template setVal<T>(other->template load<T>(), overflowBuffer);
        },
        [&](auto) -> void {
            throw RuntimeException(unsupportedTypeError(LogicalType(other->storedType)));
        });
}

// -----------------------------------------------------------------------------
// JSON total order
// -----------------------------------------------------------------------------
// _GQL_MAX/_GQL_MIN parse JSON text and order values under the GQL/Cypher
// total order. Cross-class ranking (ascending):
//
//   null(0) < bool(1) < array(2) < string(3) < number(4) < object(5)
//
// The relative order of array/string/number is pinned by TCK
// test/tck/features/expressions/aggregation/Aggregation2.feature:
//   [12] min([1,'a',null,[1,2],0.2,'b']) = [1,2]
//        => array < string and array < number
//   [11] max([1,'a',null,[1,2],0.2,'b']) = 1
//        => string < number
// i.e. array < string < number — the reverse of CIP2016-06-14's
// Number < String < List rank for that block. Booleans and objects keep their
// CIP2016-06-14 positions relative to the block (Boolean below everything,
// Map/object above everything), which CIP does not conflict with here; JSON
// null is the lowest value (reachable only inside nested arrays, and matching
// the engine's list comparison, which orders null below every non-null
// element). Un-orderable pairs (distinct objects) throw rather than guess.
//
// Within a class:
//   numbers  — int/real/uint compare numerically regardless of text form
//              (TCK [5]/[6]) through the exact decimal path shared with the
//              order predicates below (compareDecimalTexts — never double), so
//              _GQL_MAX/_GQL_MIN cannot disagree with _GQL_LT.._GQL_NE or
//              ORDER BY _GQL_SORTKEY on the same values.
//   strings  — byte-wise lexicographic (TCK [7]/[8]: 'b' > 'B' > 'abc').
//   arrays   — element-wise lexicographic, shorter prefix is smaller
//              (TCK [9]/[10]: [1] < [2] < [2,1]).
//   bools    — false < true.
int jsonRank(yyjson_val* val) {
    if (yyjson_is_null(val)) {
        return 0;
    }
    if (yyjson_is_bool(val)) {
        return 1;
    }
    if (yyjson_is_arr(val)) {
        return 2;
    }
    if (yyjson_is_str(val)) {
        return 3;
    }
    if (yyjson_is_num(val)) {
        return 4;
    }
    if (yyjson_is_obj(val)) {
        return 5;
    }
    throw RuntimeException("_GQL_MAX/_GQL_MIN: unsupported JSON value in comparison");
}

// Exact decimal machinery, shared with the order-predicate bridge further
// down this file (same anonymous namespace; defined below). Declared here so
// aggregate-side number comparison routes through the identical path.
std::string parsedNumberText(yyjson_val* root, const char* errorPrefix);
int compareDecimalTexts(std::string_view lhs, std::string_view rhs, const char* errorPrefix);

// Numbers compare through exact decimal normalization — the same
// compareDecimalTexts path the order predicates and _GQL_SORTKEY use — so
// min/max can never disagree with _GQL_GT/ORDER BY on the same data. Integers
// keep their exact spelling; reals carry their double's shortest round-trip
// form (the shared ceiling for numbers that only exist as parsed doubles).
int compareJsonNumbers(yyjson_val* lhs, yyjson_val* rhs) {
    static constexpr const char* kPrefix = "_GQL_MAX/_GQL_MIN";
    return compareDecimalTexts(parsedNumberText(lhs, kPrefix), parsedNumberText(rhs, kPrefix),
        kPrefix);
}

// Structural equality over parsed JSON (all-inline yyjson API, no link-time
// dependency on the yyjson library): used so equal objects nested inside
// arrays compare equal instead of tripping the object-order guard below.
bool jsonDeepEquals(yyjson_val* lhs, yyjson_val* rhs) {
    if (jsonRank(lhs) != jsonRank(rhs)) {
        return false;
    }
    if (yyjson_is_null(lhs)) {
        return true;
    }
    if (yyjson_is_bool(lhs)) {
        return yyjson_get_bool(lhs) == yyjson_get_bool(rhs);
    }
    if (yyjson_is_num(lhs)) {
        return compareJsonNumbers(lhs, rhs) == 0;
    }
    if (yyjson_is_str(lhs)) {
        auto len = yyjson_get_len(lhs);
        return len == yyjson_get_len(rhs) &&
               memcmp(yyjson_get_str(lhs), yyjson_get_str(rhs), len) == 0;
    }
    if (yyjson_is_arr(lhs)) {
        auto size = yyjson_arr_size(lhs);
        if (size != yyjson_arr_size(rhs)) {
            return false;
        }
        for (auto i = 0u; i < size; ++i) {
            if (!jsonDeepEquals(yyjson_arr_get(lhs, i), yyjson_arr_get(rhs, i))) {
                return false;
            }
        }
        return true;
    }
    auto size = yyjson_obj_size(lhs);
    if (size != yyjson_obj_size(rhs)) {
        return false;
    }
    yyjson_val* key = nullptr;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(lhs, &iter);
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
        auto lhsVal = yyjson_obj_iter_get_val(key);
        auto rhsVal = yyjson_obj_getn(rhs, yyjson_get_str(key), yyjson_get_len(key));
        if (rhsVal == nullptr || !jsonDeepEquals(lhsVal, rhsVal)) {
            return false;
        }
    }
    return true;
}

int compareJsonValues(yyjson_val* lhs, yyjson_val* rhs) {
    auto lhsRank = jsonRank(lhs);
    auto rhsRank = jsonRank(rhs);
    if (lhsRank != rhsRank) {
        return lhsRank < rhsRank ? -1 : 1;
    }
    if (yyjson_is_null(lhs)) {
        return 0;
    }
    if (yyjson_is_bool(lhs)) {
        auto l = yyjson_get_bool(lhs);
        auto r = yyjson_get_bool(rhs);
        return l == r ? 0 : (l ? 1 : -1);
    }
    if (yyjson_is_arr(lhs)) {
        auto lhsSize = yyjson_arr_size(lhs);
        auto rhsSize = yyjson_arr_size(rhs);
        auto commonSize = std::min(lhsSize, rhsSize);
        for (auto i = 0u; i < commonSize; ++i) {
            auto cmp = compareJsonValues(yyjson_arr_get(lhs, i), yyjson_arr_get(rhs, i));
            if (cmp != 0) {
                return cmp;
            }
        }
        // Shorter prefix sorts smaller (Cypher list order).
        return lhsSize < rhsSize ? -1 : (lhsSize > rhsSize ? 1 : 0);
    }
    if (yyjson_is_str(lhs)) {
        auto lhsLen = yyjson_get_len(lhs);
        auto rhsLen = yyjson_get_len(rhs);
        auto cmp = memcmp(yyjson_get_str(lhs), yyjson_get_str(rhs), std::min(lhsLen, rhsLen));
        if (cmp != 0) {
            return cmp;
        }
        return lhsLen < rhsLen ? -1 : (lhsLen > rhsLen ? 1 : 0);
    }
    if (yyjson_is_num(lhs)) {
        return compareJsonNumbers(lhs, rhs);
    }
    // Objects: equal content is fine, distinct content is un-orderable.
    if (jsonDeepEquals(lhs, rhs)) {
        return 0;
    }
    throw RuntimeException(
        "_GQL_MAX/_GQL_MIN: JSON objects do not have a well-defined total order");
}

std::string jsonTextSnippet(const string_t& val) {
    constexpr uint32_t MAX_SNIPPET = 64;
    auto len = std::min<uint32_t>(val.len, MAX_SNIPPET);
    std::string snippet(reinterpret_cast<const char*>(val.getData()), len);
    if (val.len > MAX_SNIPPET) {
        snippet += "...";
    }
    return snippet;
}

// -1 / 0 / +1 under the order documented at compareJsonValues. Throws loudly
// on invalid JSON text or un-orderable content — never a silent wrong answer.
int compareJsonTexts(const string_t& lhs, const string_t& rhs) {
    if (lhs.len == rhs.len && memcmp(lhs.getData(), rhs.getData(), lhs.len) == 0) {
        return 0;
    }
    auto parse = [](const string_t& val) {
        auto doc = stringToJsonNoError(
            std::string(reinterpret_cast<const char*>(val.getData()), val.len));
        if (doc.ptr == nullptr) {
            throw RuntimeException(
                std::format("_GQL_MAX/_GQL_MIN: invalid JSON value: {}", jsonTextSnippet(val)));
        }
        return doc;
    };
    auto lhsDoc = parse(lhs);
    auto rhsDoc = parse(rhs);
    return compareJsonValues(yyjson_doc_get_root(lhsDoc.ptr), yyjson_doc_get_root(rhsDoc.ptr));
}

// -----------------------------------------------------------------------------
// Update / combine
// -----------------------------------------------------------------------------

// True when the candidate value at `pos` beats the state's current winner.
// IS_MAX=true orders with GreaterThan (built-in MAX semantics), false with
// LessThan (built-in MIN semantics).
template<bool IS_MAX>
bool candidateWins(GqlMinMaxState* state, ValueVector* input, uint32_t pos) {
    if (state->storedType == LogicalTypeID::JSON) {
        auto candidate = input->getValue<string_t>(pos);
        auto winner = state->load<string_t>();
        auto cmp = compareJsonTexts(candidate, winner);
        return IS_MAX ? cmp > 0 : cmp < 0;
    }
    using OP = std::conditional_t<IS_MAX, GreaterThan, LessThan>;
    return TypeUtils::visit(input->dataType.getPhysicalType(),
        [&]<ComparableTypes T>(T) {
            uint8_t cmp = 0;
            OP::template operation<T, T>(input->getValue<T>(pos), state->template load<T>(), cmp,
                nullptr /* leftVector */, nullptr /* rightVector */);
            return cmp != 0;
        },
        [&](auto) -> bool { throw RuntimeException(unsupportedTypeError(input->dataType)); });
}

// True when `other`'s stored value beats `state`'s current winner.
template<bool IS_MAX>
bool otherWins(GqlMinMaxState* state, const GqlMinMaxState* other) {
    if (state->storedType == LogicalTypeID::JSON) {
        auto cmp = compareJsonTexts(other->load<string_t>(), state->load<string_t>());
        return IS_MAX ? cmp > 0 : cmp < 0;
    }
    using OP = std::conditional_t<IS_MAX, GreaterThan, LessThan>;
    return TypeUtils::visit(LogicalType::getPhysicalType(state->storedType),
        [&]<ComparableTypes T>(T) {
            uint8_t cmp = 0;
            OP::template operation<T, T>(other->template load<T>(), state->template load<T>(), cmp,
                nullptr /* leftVector */, nullptr /* rightVector */);
            return cmp != 0;
        },
        [&](auto) -> bool {
            throw RuntimeException(unsupportedTypeError(LogicalType(state->storedType)));
        });
}

template<bool IS_MAX>
void updateSingle(GqlMinMaxState* state, ValueVector* input, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    if (input->isNull(pos)) {
        return;
    }
    // Reject unsupported types on the first real value: all-null inputs of an
    // unknown type still produce NULL (no comparison happened), anything else
    // must fail loudly instead of guessing an order.
    if (!isSupportedInput(input->dataType)) {
        throw RuntimeException(unsupportedTypeError(input->dataType));
    }
    if (state->isNull) {
        storeFromVector(state, input, pos, overflowBuffer);
        state->isNull = false;
        return;
    }
    if (state->storedType != input->dataType.getLogicalTypeID()) {
        throw RuntimeException(std::format("_GQL_MAX/_GQL_MIN: cannot compare {} with {}",
            LogicalTypeUtils::toString(state->storedType),
            LogicalTypeUtils::toString(input->dataType.getLogicalTypeID())));
    }
    if (candidateWins<IS_MAX>(state, input, pos)) {
        storeFromVector(state, input, pos, overflowBuffer);
    }
}

template<bool IS_MAX>
void updateAll(uint8_t* state_, ValueVector* input, uint64_t /*multiplicity*/,
    InMemOverflowBuffer* overflowBuffer) {
    DASSERT(!input->state->isFlat());
    auto* state = reinterpret_cast<GqlMinMaxState*>(state_);
    input->forEachNonNull(
        [&](auto pos) { updateSingle<IS_MAX>(state, input, pos, overflowBuffer); });
}

template<bool IS_MAX>
void updatePos(uint8_t* state_, ValueVector* input, uint64_t /*multiplicity*/, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    // The caller filters nulls, but filtering here too is harmless.
    updateSingle<IS_MAX>(reinterpret_cast<GqlMinMaxState*>(state_), input, pos, overflowBuffer);
}

template<bool IS_MAX>
void combine(uint8_t* state_, uint8_t* otherState_, InMemOverflowBuffer* overflowBuffer) {
    auto* other = reinterpret_cast<GqlMinMaxState*>(otherState_);
    if (other->isNull) {
        return;
    }
    auto* state = reinterpret_cast<GqlMinMaxState*>(state_);
    if (state->isNull) {
        storeFromState(state, other, overflowBuffer);
        state->isNull = false;
        return;
    }
    if (state->storedType != other->storedType) {
        throw RuntimeException(std::format("_GQL_MAX/_GQL_MIN: cannot combine {} with {}",
            LogicalTypeUtils::toString(state->storedType),
            LogicalTypeUtils::toString(other->storedType)));
    }
    if (otherWins<IS_MAX>(state, other)) {
        storeFromState(state, other, overflowBuffer);
    }
}

std::unique_ptr<AggregateState> initialize() {
    return std::make_unique<GqlMinMaxState>();
}

// No-op: empty / all-null groups stay NULL through the default
// AggregateStateWithNull.isNull path (needToHandleNulls stays false).
void finalize(uint8_t* /*state_*/) {}

// Same shape as CollectFunction::bindFunc: pin the definition's parameter to
// the concrete argument type and return the argument type unchanged, so
// JSON->JSON, STRING->STRING, INT64->INT64, DOUBLE->DOUBLE, ...
std::unique_ptr<FunctionBindData> bindAggregate(const ScalarBindFuncInput& input) {
    DASSERT(input.arguments.size() == 1);
    auto* aggFuncDefinition = reinterpret_cast<AggregateFunction*>(input.definition);
    aggFuncDefinition->parameterTypeIDs[0] = input.arguments[0]->dataType.getLogicalTypeID();
    auto returnType = input.arguments[0]->dataType.copy();
    return std::make_unique<FunctionBindData>(std::move(returnType));
}

template<bool IS_MAX>
function_set buildMinMaxFunctionSet(const std::string& funcName) {
    function_set result;
    // Matching is exact on isDistinct (built_in_function_utils.cpp), so both
    // overloads are mandatory.
    for (auto isDistinct : std::vector<bool>{true, false}) {
        result.push_back(std::make_unique<AggregateFunction>(funcName,
            std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::ANY /* bindFunc wins */,
            initialize, updateAll<IS_MAX>, updatePos<IS_MAX>, combine<IS_MAX>, finalize,
            isDistinct, bindAggregate, nullptr /* paramRewriteFunc */));
    }
    return result;
}

// =============================================================================
// _GQL_SUM / _GQL_AVG(ANY) -> <bound result type>
// =============================================================================
// Extended GQL numeric aggregates. They take ANY so JSON columns (ANY-graph
// dynamic properties) can aggregate where the native SUM/AVG overloads reject
// JSON outright (`Function SUM did not receive correct arguments: Actual:
// (JSON)`). bindFunc pins the result type from the argument:
//
//   signed integers (incl. SERIAL) -> SUM: INT128, AVG: DOUBLE
//   unsigned integers              -> SUM: UINT128, AVG: DOUBLE
//   FLOAT / DOUBLE                 -> SUM: DOUBLE, AVG: DOUBLE
//   JSON                           -> SUM: JSON, AVG: JSON (JSON number text)
//   anything else                  -> rejected loudly at bind time
//
// Value semantics mirror the engine's SumFunction/AvgFunction
// (src/include/function/aggregate/sum.h, avg.h): SQL NULLs and JSON `null`
// contribute nothing, multiplicity re-adds the value in a loop (never a
// multiply — same rounding as the native sum), an empty or all-NULL group
// keeps AggregateStateWithNull.isNull so the framework writes NULL (never a
// synthesized 0), and AVG divides by the multiplicity-weighted count of
// contributing values. JSON numbers accumulate with full integer tracking:
// integers stay exact in an int128_t (every uint64 yyjson can emit fits),
// reals go to a double and set sawReal — a sum that never saw a real keeps
// exact integer text (`75`, `-3`), a mixed sum renders as a JSON real
// (`2.0`, `37.5`), and an AVG is always a JSON real. Non-numeric JSON,
// unparsable text and non-finite results throw with the `_GQL_SUM/_GQL_AVG:`
// prefix instead of guessing.

std::string sumAvgUnsupportedTypeError(const LogicalType& type) {
    return std::format("_GQL_SUM/_GQL_AVG: unsupported input type {}",
        LogicalTypeUtils::toString(type.getLogicalTypeID()));
}

// Renders a double the way yyjson writes JSON reals: shortest round-trip
// digits with an explicit fraction for integral values (`2.0`, not `2`;
// `3.5` stays `3.5`, never printf padding). Non-finite sums have no JSON
// representation and fail loudly rather than emitting text that only breaks
// downstream parsers.
std::string formatJsonReal(double val) {
    if (!std::isfinite(val)) {
        throw RuntimeException(std::format(
            "_GQL_SUM/_GQL_AVG: non-finite value cannot be encoded as JSON: {}", val));
    }
    auto text = std::format("{}", val);
    if (text.find_first_of(".e") == std::string::npos) {
        text += ".0";
    }
    return text;
}

// Local mirrors of CastInt128ToFloating / CastUint128ToFloating
// (src/common/types/int128_t.cpp): AVG must fold an integer sum exactly the
// way the engine's native AvgState::finalize does for the same values, and
// the Int128_t helper class is not part of the extension-facing API surface.
template<typename REAL_T>
REAL_T int128ToFloating(int128_t input) {
    if (input.high == -1) {
        // The default branch's (2^64 - 1) multiplier overshoots by one for
        // values in [-2^64, -1]; negate the low half exactly instead.
        return -static_cast<REAL_T>(UINT64_MAX - input.low) - 1;
    }
    return static_cast<REAL_T>(input.high) * static_cast<REAL_T>(UINT64_MAX) +
           static_cast<REAL_T>(input.low);
}

template<typename REAL_T>
REAL_T uint128ToFloating(uint128_t input) {
    return static_cast<REAL_T>(input.high) * static_cast<REAL_T>(UINT64_MAX) +
           static_cast<REAL_T>(input.low);
}

// Aggregate state for _GQL_SUM/_GQL_AVG.
//
// Contract (see AggregateState in function/aggregate_function.h): states are
// memcpy'd into factorized tables and their destructors never run, so every
// member must be trivially destructible — no strings live here, JSON text is
// parsed per value and only accumulators are kept. `storedType` is the bound
// argument's logical type: one query binds one type (bindFunc rejects
// everything else), so it is constant for the lifetime of a non-null state;
// ANY only ever appears while `isNull` is still true. Unused accumulators
// stay zero for the input family in play, which makes combine a plain
// field-wise add. `count` and `sawReal` are read only by their own
// instantiation (AVG and JSON respectively).
template<bool IS_AVG>
struct GqlSumAvgState : public AggregateStateWithNull {
    LogicalTypeID storedType = LogicalTypeID::ANY;
    int128_t intSum{};    // signed int inputs; JSON integer part
    uint128_t uintSum{};  // unsigned int inputs
    double dblSum{};      // FLOAT/DOUBLE inputs; JSON real part
    uint64_t count{};     // multiplicity-weighted count of contributing values (AVG)
    bool sawReal{};       // JSON: a real number was accumulated

    uint32_t getStateSize() const override { return sizeof(*this); }
    void writeToVector(ValueVector* outputVector, uint64_t pos) override;
};

template<bool IS_AVG>
void GqlSumAvgState<IS_AVG>::writeToVector(ValueVector* outputVector, uint64_t pos) {
    if (storedType == LogicalTypeID::ANY) {
        throw RuntimeException("_GQL_SUM/_GQL_AVG: no value to materialize");
    }
    if (storedType == LogicalTypeID::JSON) {
        std::string text;
        if constexpr (IS_AVG) {
            // GQL/Cypher avg is always a real, even over whole numbers.
            text = formatJsonReal(static_cast<double>(
                (int128ToFloating<long double>(intSum) + static_cast<long double>(dblSum)) /
                static_cast<long double>(count)));
        } else if (sawReal) {
            // Mixed int/real sums fold the exact integer part in once — the
            // same conversion the engine's own int128 casts use.
            text = formatJsonReal(int128ToFloating<double>(intSum) + dblSum);
        } else {
            // All-integer sums keep exact decimal text (`75`, `-3`), never
            // routed through double.
            text = TypeUtils::toString(intSum);
        }
        StringVector::addString(outputVector, pos, text);
        return;
    }
    // Typed results follow the type bindFunc pinned: AVG is always DOUBLE,
    // SUM keeps the accumulator matching the input family (SumState writes
    // its accumulator the same way, src/include/function/aggregate/sum.h:14).
    TypeUtils::visit(LogicalType(storedType),
        [&]<SignedIntegerTypes T>(T) {
            if constexpr (IS_AVG) {
                outputVector->setValue(pos, static_cast<double>(
                    int128ToFloating<long double>(intSum) / static_cast<long double>(count)));
            } else {
                outputVector->setValue(pos, intSum);
            }
        },
        [&]<UnsignedIntegerTypes T>(T) {
            if constexpr (IS_AVG) {
                outputVector->setValue(pos, static_cast<double>(
                    uint128ToFloating<long double>(uintSum) / static_cast<long double>(count)));
            } else {
                outputVector->setValue(pos, uintSum);
            }
        },
        [&]<FloatingPointTypes T>(T) {
            // Same expression as AvgState<FloatingPointTypes>::finalize
            // (avg.h:34): sum / count in double. Unlike the engine's IEEE
            // write-through, a non-finite result is a loud reject here — the
            // JSON result path (formatJsonReal) already rejects, and a silent
            // Inf/NaN would be a wrong GQL answer.
            double out = IS_AVG ? dblSum / count : dblSum;
            if (!std::isfinite(out)) {
                throw RuntimeException(
                    std::format("_GQL_SUM/_GQL_AVG: non-finite result: {}", out));
            }
            outputVector->setValue(pos, out);
        },
        [&](auto) -> void {
            throw RuntimeException(sumAvgUnsupportedTypeError(LogicalType(storedType)));
        });
}

// Parses one JSON input value (same stringToJsonNoError path as
// compareJsonTexts, inline yyjson accessors like compareJsonNumbers) and
// folds it into the state. JSON `null` means "no value": a group of only
// JSON nulls stays NULL, matching the SQL NULL path. Multiplicity re-adds
// the value exactly like SumFunction::updateSingleValue (sum.h:42-48), and
// the first contribution assigns rather than adds so a lone -0.0 keeps its
// sign. Non-numeric content throws loudly — never a silent wrong answer.
template<bool IS_AVG>
void accumulateJson(GqlSumAvgState<IS_AVG>* state, const string_t& value, uint64_t multiplicity) {
    auto doc = stringToJsonNoError(
        std::string(reinterpret_cast<const char*>(value.getData()), value.len));
    if (doc.ptr == nullptr) {
        throw RuntimeException(
            std::format("_GQL_SUM/_GQL_AVG: invalid JSON value: {}", jsonTextSnippet(value)));
    }
    auto* root = yyjson_doc_get_root(doc.ptr);
    if (yyjson_is_null(root)) {
        return;
    }
    if (yyjson_is_real(root)) {
        auto val = yyjson_get_real(root);
        for (uint64_t j = 0; j < multiplicity; ++j) {
            if (state->isNull) {
                state->dblSum = val;
                state->isNull = false;
            } else {
                Add::operation(state->dblSum, val, state->dblSum);
            }
            state->sawReal = true;
        }
    } else if (yyjson_is_int(root)) {
        // yyjson only emits uint64 or sint64 integers with default read
        // flags; both fit an int128, so integer JSON accumulates exactly.
        int128_t val = yyjson_is_uint(root) ? int128_t(yyjson_get_uint(root)) :
                                              int128_t(yyjson_get_sint(root));
        for (uint64_t j = 0; j < multiplicity; ++j) {
            if (state->isNull) {
                state->intSum = val;
                state->isNull = false;
            } else {
                Add::operation(state->intSum, val, state->intSum);
            }
        }
    } else {
        throw RuntimeException(std::format(
            "_GQL_SUM/_GQL_AVG: non-numeric JSON value in aggregate: {}", jsonTextSnippet(value)));
    }
    if constexpr (IS_AVG) {
        state->count += multiplicity;
    }
}

template<bool IS_AVG>
void updateSumAvgSingle(GqlSumAvgState<IS_AVG>* state, ValueVector* input, uint32_t pos,
    uint64_t multiplicity) {
    if (input->isNull(pos)) {
        return;
    }
    const auto typeID = input->dataType.getLogicalTypeID();
    if (state->isNull) {
        state->storedType = typeID;
    } else if (state->storedType != typeID) {
        // Unreachable through the binder (bindFunc pins one concrete type per
        // query); guarded anyway in the same shape as _GQL_MAX/_GQL_MIN.
        throw RuntimeException(std::format("_GQL_SUM/_GQL_AVG: cannot aggregate {} with {}",
            LogicalTypeUtils::toString(state->storedType), LogicalTypeUtils::toString(typeID)));
    }
    if (typeID == LogicalTypeID::JSON) {
        accumulateJson(state, input->getValue<string_t>(pos), multiplicity);
        return;
    }
    // Typed inputs accumulate into the family's wide accumulator, mirroring
    // SumFunction::updateSingleValue: loop multiplicity times, assigning on
    // the first contribution and Add-ing after (sum.h:42-48). The fallback
    // only fires for types bindFunc already rejected — kept so an
    // unsupported value fails loudly instead of silently skipping.
    TypeUtils::visit(input->dataType,
        [&]<SignedIntegerTypes T>(T) {
            T val = input->getValue<T>(pos);
            for (uint64_t j = 0; j < multiplicity; ++j) {
                if (state->isNull) {
                    state->intSum = val;
                    state->isNull = false;
                } else {
                    Add::operation(state->intSum, val, state->intSum);
                }
            }
        },
        [&]<UnsignedIntegerTypes T>(T) {
            T val = input->getValue<T>(pos);
            for (uint64_t j = 0; j < multiplicity; ++j) {
                if (state->isNull) {
                    state->uintSum = val;
                    state->isNull = false;
                } else {
                    Add::operation(state->uintSum, val, state->uintSum);
                }
            }
        },
        [&]<FloatingPointTypes T>(T) {
            T val = input->getValue<T>(pos);
            for (uint64_t j = 0; j < multiplicity; ++j) {
                if (state->isNull) {
                    state->dblSum = val;
                    state->isNull = false;
                } else {
                    Add::operation(state->dblSum, val, state->dblSum);
                }
            }
        },
        [&](auto) -> void {
            throw RuntimeException(sumAvgUnsupportedTypeError(input->dataType));
        });
    if constexpr (IS_AVG) {
        // AvgFunction counts each non-null value's multiplicity (avg.h:75).
        state->count += multiplicity;
    }
}

template<bool IS_AVG>
void sumAvgUpdateAll(uint8_t* state_, ValueVector* input, uint64_t multiplicity,
    InMemOverflowBuffer* /*overflowBuffer*/) {
    DASSERT(!input->state->isFlat());
    auto* state = reinterpret_cast<GqlSumAvgState<IS_AVG>*>(state_);
    input->forEachNonNull(
        [&](auto pos) { updateSumAvgSingle<IS_AVG>(state, input, pos, multiplicity); });
}

template<bool IS_AVG>
void sumAvgUpdatePos(uint8_t* state_, ValueVector* input, uint64_t multiplicity, uint32_t pos,
    InMemOverflowBuffer* /*overflowBuffer*/) {
    // The caller filters nulls, but filtering here too is harmless (same as
    // _GQL_MAX/_GQL_MIN's updatePos).
    updateSumAvgSingle<IS_AVG>(reinterpret_cast<GqlSumAvgState<IS_AVG>*>(state_), input, pos,
        multiplicity);
}

// Field-wise merge shaped like SumFunction::combine (sum.h:52-65): null
// short-circuits, a null receiver adopts the other state wholesale, and two
// live states add. Accumulators the input family never touched are zero on
// both sides, so adding all of them is safe; sawReal and count merge with
// OR / + so a JSON sum stays exact-integer on both sides and a mixed sum
// keeps its real flag across partitions.
template<bool IS_AVG>
void sumAvgCombine(uint8_t* state_, uint8_t* otherState_,
    InMemOverflowBuffer* /*overflowBuffer*/) {
    auto* other = reinterpret_cast<GqlSumAvgState<IS_AVG>*>(otherState_);
    if (other->isNull) {
        return;
    }
    auto* state = reinterpret_cast<GqlSumAvgState<IS_AVG>*>(state_);
    if (state->isNull) {
        state->storedType = other->storedType;
        state->intSum = other->intSum;
        state->uintSum = other->uintSum;
        state->dblSum = other->dblSum;
        state->count = other->count;
        state->sawReal = other->sawReal;
        state->isNull = false;
        return;
    }
    if (state->storedType != other->storedType) {
        throw RuntimeException(std::format("_GQL_SUM/_GQL_AVG: cannot combine {} with {}",
            LogicalTypeUtils::toString(state->storedType),
            LogicalTypeUtils::toString(other->storedType)));
    }
    Add::operation(state->intSum, other->intSum, state->intSum);
    Add::operation(state->uintSum, other->uintSum, state->uintSum);
    Add::operation(state->dblSum, other->dblSum, state->dblSum);
    state->count += other->count;
    state->sawReal = state->sawReal || other->sawReal;
}

template<bool IS_AVG>
std::unique_ptr<AggregateState> sumAvgInitialize() {
    return std::make_unique<GqlSumAvgState<IS_AVG>>();
}

// Same shape as bindAggregate above: pin the definition's parameter to the
// concrete argument type and return the result type from the matrix in the
// section header. Unresolved ANY placeholders (prepared statements) keep an
// ANY result and are re-bound with concrete types before execution — the
// AggregateFunctionExpression::cast path _GQL_MAX/_GQL_MIN also relies on.
// Every concrete type outside the matrix fails loudly here: the ANY wildcard
// matched the overload, so this check is the only gate before execution.
template<bool IS_AVG>
std::unique_ptr<FunctionBindData> bindSumAvg(const ScalarBindFuncInput& input) {
    DASSERT(input.arguments.size() == 1);
    auto* aggFuncDefinition = reinterpret_cast<AggregateFunction*>(input.definition);
    const auto& argType = input.arguments[0]->dataType;
    aggFuncDefinition->parameterTypeIDs[0] = argType.getLogicalTypeID();
    if (argType.getLogicalTypeID() == LogicalTypeID::ANY) {
        return std::make_unique<FunctionBindData>(argType.copy());
    }
    LogicalTypeID resultTypeID;
    switch (argType.getLogicalTypeID()) {
    case LogicalTypeID::JSON:
        resultTypeID = LogicalTypeID::JSON;
        break;
    case LogicalTypeID::INT8:
    case LogicalTypeID::INT16:
    case LogicalTypeID::INT32:
    case LogicalTypeID::INT64:
    case LogicalTypeID::INT128:
    case LogicalTypeID::SERIAL:
        resultTypeID = IS_AVG ? LogicalTypeID::DOUBLE : LogicalTypeID::INT128;
        break;
    case LogicalTypeID::UINT8:
    case LogicalTypeID::UINT16:
    case LogicalTypeID::UINT32:
    case LogicalTypeID::UINT64:
    case LogicalTypeID::UINT128:
        resultTypeID = IS_AVG ? LogicalTypeID::DOUBLE : LogicalTypeID::UINT128;
        break;
    case LogicalTypeID::FLOAT:
    case LogicalTypeID::DOUBLE:
        // Matches appendSumOrAvgFuncs: floats sum as DOUBLE, avg is always
        // DOUBLE (src/function/aggregate_function.cpp:32-59).
        resultTypeID = LogicalTypeID::DOUBLE;
        break;
    default:
        throw BinderException(sumAvgUnsupportedTypeError(argType));
    }
    return std::make_unique<FunctionBindData>(LogicalType(resultTypeID));
}

template<bool IS_AVG>
function_set buildSumAvgFunctionSet(const std::string& funcName) {
    function_set result;
    // Matching is exact on isDistinct (built_in_function_utils.cpp), so both
    // overloads are mandatory; the executor's distinct hash table does the
    // deduplication, the callbacks never see repeats.
    for (auto isDistinct : std::vector<bool>{true, false}) {
        result.push_back(std::make_unique<AggregateFunction>(funcName,
            std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::ANY /* bindFunc wins */,
            sumAvgInitialize<IS_AVG>, sumAvgUpdateAll<IS_AVG>, sumAvgUpdatePos<IS_AVG>,
            sumAvgCombine<IS_AVG>, finalize /* no-op, shared with _GQL_MAX/_GQL_MIN above */,
            isDistinct, bindSumAvg<IS_AVG>, nullptr /* paramRewriteFunc */));
    }
    return result;
}

} // namespace

function_set GqlMaxFunction::getFunctionSet() {
    return buildMinMaxFunctionSet<true>(name);
}

function_set GqlMinFunction::getFunctionSet() {
    return buildMinMaxFunctionSet<false>(name);
}

function_set GqlSumFunction::getFunctionSet() {
    return buildSumAvgFunctionSet<false>(name);
}

function_set GqlAvgFunction::getFunctionSet() {
    return buildSumAvgFunctionSet<true>(name);
}

// =============================================================================
// _GQL_LT / _GQL_LE / _GQL_GT / _GQL_GE(ANY, ANY) -> BOOL
// _GQL_SORTKEY(ANY) -> STRING
// =============================================================================
// The Q2-A comparison bridge. Native comparisons on JSON-typed columns order
// the stored text byte-wise, which is silently wrong for ANY-graph properties:
// one column mixes numbers, strings, arrays and nulls, and text order is not
// the GQL total order. These functions replace it with the Phase 8 order:
//
//   null(0) < bool(1) < array(2) < string(3) < number(4) < object(5)
//
// Operands are classified from their logical type, never from their text
// alone: a STRING column is a string, a JSON column is parsed (and — when
// parsing fails — is the bare text the property writer stores, `x` and not
// `"x"`), LIST/ARRAY columns are serialized through jsonify, and typed
// integers/floats keep an exact decimal spelling. Numbers compare through
// exact decimal normalization (arbitrary length, never double) — the same
// path compareJsonNumbers (aggregate min/max) now uses, so every number entry
// point agrees on one order.
//
// _GQL_SORTKEY encodes the same order into one byte-comparable STRING so
// ORDER BY can use it directly (the engine compares STRING values byte-wise).

namespace {

constexpr const char* ORDER_PREDICATE_PREFIX = "_GQL_LT/_GQL_LE/_GQL_GT/_GQL_GE";
constexpr const char* SORTKEY_PREFIX = "_GQL_SORTKEY";

// -----------------------------------------------------------------------------
// Exact decimal normalization (shared by the predicates, the sort key and
// aggregate min/max number comparison)
// -----------------------------------------------------------------------------

// `value = sign * 0.<digits> * 10^exponent`, with `digits` stripped of both
// leading and trailing zeros (zero is sign 0 with empty digits). Parses JSON
// number spellings (fraction, `e`/`E` exponent, exponent sign) as well as the
// plain integer text TypeUtils::toString produces. All digits are kept: no
// step routes the value through double — which is why every number-ordering
// entry point (compareJsonNumbers included) funnels through this path.
struct DecimalParts {
    int32_t sign = 0;
    std::string digits;
    int64_t exponent = 0;
};

bool allDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (auto c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

int64_t checkedAddInt64(int64_t lhs, int64_t rhs, const char* errorPrefix) {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)) {
        throw RuntimeException(std::format("{}: number exponent out of range", errorPrefix));
    }
    return lhs + rhs;
}

int64_t parseExponent(std::string_view text, const char* errorPrefix) {
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (!allDigits(text)) {
        throw RuntimeException(std::format("{}: invalid number text", errorPrefix));
    }
    int64_t value = 0;
    for (auto c : text) {
        int64_t digit = c - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
            throw RuntimeException(std::format("{}: number exponent out of range", errorPrefix));
        }
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

DecimalParts normalizeDecimalText(std::string_view text, const char* errorPrefix) {
    DecimalParts result;
    result.sign = 1;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        result.sign = text.front() == '-' ? -1 : 1;
        text.remove_prefix(1);
    }
    auto exponentPos = text.find_first_of("eE");
    auto mantissa = exponentPos == std::string_view::npos ? text : text.substr(0, exponentPos);
    auto exponent = exponentPos == std::string_view::npos ?
                        int64_t{0} :
                        parseExponent(text.substr(exponentPos + 1), errorPrefix);
    auto dotPos = mantissa.find('.');
    auto intPart = dotPos == std::string_view::npos ? mantissa : mantissa.substr(0, dotPos);
    auto fracPart = dotPos == std::string_view::npos ? std::string_view{} :
                                                       mantissa.substr(dotPos + 1);
    if (!allDigits(intPart) || (dotPos != std::string_view::npos && !allDigits(fracPart))) {
        throw RuntimeException(std::format("{}: invalid number text", errorPrefix));
    }
    std::string digits(intPart);
    digits.append(fracPart.data(), fracPart.size());
    auto firstSignificant = digits.find_first_not_of('0');
    if (firstSignificant == std::string::npos) {
        // Zero (including -0 and 0.00e9): one canonical zero.
        return DecimalParts{};
    }
    auto lastSignificant = digits.find_last_not_of('0');
    result.digits = digits.substr(firstSignificant, lastSignificant + 1 - firstSignificant);
    // digits * 10^(exponent - fracLen) == 0.<digits> * 10^(digitCount + exponent -
    // fracLen); digitCount is counted from the first significant digit to the
    // end (trailing zeros included), so stripping them above keeps the value.
    auto digitCount = static_cast<int64_t>(digits.size() - firstSignificant);
    result.exponent = checkedAddInt64(checkedAddInt64(digitCount, exponent, errorPrefix),
        -static_cast<int64_t>(fracPart.size()), errorPrefix);
    return result;
}

// -1 / 0 / +1 over two decimal payload texts, exact for arbitrary lengths.
// Sign decides first; within one sign the magnitude (exponent, then canonical
// digit string — the shorter prefix is smaller) decides, and negative
// operands reverse it. The one number-ordering path: predicates, sort key
// and compareJsonNumbers (aggregate min/max) all land here.
int compareDecimalTexts(std::string_view lhs, std::string_view rhs, const char* errorPrefix) {
    auto lhsParts = normalizeDecimalText(lhs, errorPrefix);
    auto rhsParts = normalizeDecimalText(rhs, errorPrefix);
    if (lhsParts.sign != rhsParts.sign) {
        return lhsParts.sign < rhsParts.sign ? -1 : 1;
    }
    if (lhsParts.sign == 0) {
        return 0;
    }
    int cmp = 0;
    if (lhsParts.exponent != rhsParts.exponent) {
        cmp = lhsParts.exponent < rhsParts.exponent ? -1 : 1;
    } else {
        auto commonSize = std::min(lhsParts.digits.size(), rhsParts.digits.size());
        cmp = memcmp(lhsParts.digits.data(), rhsParts.digits.data(), commonSize);
        if (cmp == 0) {
            cmp = lhsParts.digits.size() < rhsParts.digits.size() ?
                      -1 :
                      (lhsParts.digits.size() > rhsParts.digits.size() ? 1 : 0);
        }
        cmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    return lhsParts.sign < 0 ? -cmp : cmp;
}

// Byte-wise string order (TCK Aggregation2 [7]/[8]), shorter prefix smaller.
int compareOperandTexts(const std::string& lhs, const std::string& rhs) {
    auto commonSize = std::min(lhs.size(), rhs.size());
    auto cmp = memcmp(lhs.data(), rhs.data(), commonSize);
    if (cmp != 0) {
        return cmp < 0 ? -1 : 1;
    }
    return lhs.size() < rhs.size() ? -1 : (lhs.size() > rhs.size() ? 1 : 0);
}

// -----------------------------------------------------------------------------
// Operand classification
// -----------------------------------------------------------------------------

// One vector value classified into the GQL total order. `text` carries the
// STRING contents or the NUMBER exact decimal text; `jsonValue` (and the
// `jsonDoc` keeping it alive) carries a parsed ARRAY — recursed into — or
// OBJECT, whose only total-order content is deep equality.
struct GqlOrderedOperand {
    int32_t rank = 0;
    bool boolValue = false;
    std::string text;
    yyjson_val* jsonValue = nullptr;
    std::optional<JsonWrapper> jsonDoc;
};

GqlOrderedOperand makeNumberOperand(std::string text) {
    GqlOrderedOperand result;
    result.rank = 4;
    result.text = std::move(text);
    return result;
}

// Typed FLOAT/DOUBLE payloads reuse formatJsonReal's shortest round-trip
// spelling (exact for ordering: the shortest form uniquely identifies the
// double). The finiteness rejection is repeated here so the error carries
// this bridge's own prefix instead of formatJsonReal's _GQL_SUM/_GQL_AVG one.
std::string realToNumberText(double value, const char* errorPrefix) {
    if (!std::isfinite(value)) {
        throw RuntimeException(
            std::format("{}: non-finite value cannot be ordered: {}", errorPrefix, value));
    }
    return formatJsonReal(value);
}

// Nested numbers inside an already-parsed document have no source text left;
// render the parsed value losslessly instead (integers stay exact, a real
// keeps its double's shortest round-trip form).
std::string parsedNumberText(yyjson_val* root, const char* errorPrefix) {
    if (yyjson_is_uint(root)) {
        return TypeUtils::toString(yyjson_get_uint(root));
    }
    if (yyjson_is_sint(root)) {
        return TypeUtils::toString(yyjson_get_sint(root));
    }
    return realToNumberText(yyjson_get_real(root), errorPrefix);
}

// Classifies one node of a parsed document. `numberToken` is the untouched
// source text when the root spans the whole input (top-level numbers), so the
// exact spelling survives yyjson's double folding; empty for nested values.
GqlOrderedOperand classifyParsedJson(yyjson_val* root, std::string_view numberToken,
    const char* errorPrefix) {
    GqlOrderedOperand result;
    result.rank = jsonRank(root);
    switch (result.rank) {
    case 0:
        // JSON null is a value below everything (never SQL NULL).
        break;
    case 1:
        result.boolValue = yyjson_get_bool(root);
        break;
    case 2:
    case 5:
        result.jsonValue = root;
        break;
    case 3:
        result.text.assign(yyjson_get_str(root), yyjson_get_len(root));
        break;
    case 4:
        if (!numberToken.empty()) {
            // The root spans the whole input, so trimming JSON whitespace
            // leaves exactly the source number token; normalizeDecimalText
            // validates it loudly.
            auto begin = numberToken.find_first_not_of(" \t\n\r");
            auto end = numberToken.find_last_not_of(" \t\n\r");
            auto token = numberToken.substr(begin, end - begin + 1);
            static_cast<void>(normalizeDecimalText(token, errorPrefix));
            result.text = std::string(token);
        } else {
            result.text = parsedNumberText(root, errorPrefix);
        }
        break;
    default:
        throw RuntimeException(
            std::format("{}: unsupported JSON value in comparison", errorPrefix));
    }
    return result;
}

// JSON input: parse, and fall back to the raw bare string on failure. The
// property writer stores string values without quotes (`x`), while
// _gql_to_json emits `"x"` — both forms share one column, so unparsable text
// is a string value, never an error. One exception: the engine's own
// BOOL->JSON cast spells booleans `True`/`False` (TypeUtils::toString(bool)),
// which is not JSON text — those spellings are booleans, not strings, so
// `p.flag = 'True'` stays bool-vs-string (unequal) instead of silently
// comparing as two strings. (JSON `true`/`false` from _gql_to_json already
// parse; a bare lowercase `true` is inherently ambiguous with the string
// "true" and follows the parse-first rule.)
GqlOrderedOperand classifyJsonText(std::string_view raw, const char* errorPrefix) {
    auto doc = stringToJsonNoError(std::string(raw));
    if (doc.ptr == nullptr) {
        GqlOrderedOperand result;
        result.rank = 3;
        result.text = std::string(raw);
        if (raw == "True" || raw == "False") {
            result.rank = 1;
            result.boolValue = raw == "True";
        }
        return result;
    }
    auto result = classifyParsedJson(yyjson_doc_get_root(doc.ptr), raw, errorPrefix);
    if (result.jsonValue != nullptr) {
        // emplace (not assignment): JsonWrapper is move-constructible but has
        // no move assignment operator.
        result.jsonDoc.emplace(std::move(doc));
    }
    return result;
}

// Values reached from an array: they live inside the enclosing operand's
// document, so no ownership is taken here.
GqlOrderedOperand classifyJsonValue(yyjson_val* root, const char* errorPrefix) {
    return classifyParsedJson(root, std::string_view{}, errorPrefix);
}

// Classifies one non-null vector value by its logical type. Everything the
// bridge has no faithful order for (DATE, UUID, INTERVAL, SERIAL, DECIMAL,
// STRUCT, MAP, ...) fails loudly — a loud reject beats a silent wrong answer.
GqlOrderedOperand classifyOperand(const ValueVector& vector, uint32_t pos,
    const char* errorPrefix) {
    switch (vector.dataType.getLogicalTypeID()) {
    case LogicalTypeID::JSON:
        return classifyJsonText(vector.getValue<string_t>(pos).getAsStringView(), errorPrefix);
    case LogicalTypeID::STRING: {
        GqlOrderedOperand result;
        result.rank = 3;
        result.text = vector.getValue<string_t>(pos).getAsString();
        return result;
    }
    case LogicalTypeID::BOOL: {
        GqlOrderedOperand result;
        result.rank = 1;
        result.boolValue = vector.getValue<bool>(pos);
        return result;
    }
    case LogicalTypeID::INT8:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<int8_t>(pos)));
    case LogicalTypeID::INT16:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<int16_t>(pos)));
    case LogicalTypeID::INT32:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<int32_t>(pos)));
    case LogicalTypeID::INT64:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<int64_t>(pos)));
    case LogicalTypeID::INT128:
        // Int128_t::toString — exact decimal text, never through double.
        return makeNumberOperand(TypeUtils::toString(vector.getValue<int128_t>(pos)));
    case LogicalTypeID::UINT8:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<uint8_t>(pos)));
    case LogicalTypeID::UINT16:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<uint16_t>(pos)));
    case LogicalTypeID::UINT32:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<uint32_t>(pos)));
    case LogicalTypeID::UINT64:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<uint64_t>(pos)));
    case LogicalTypeID::UINT128:
        return makeNumberOperand(TypeUtils::toString(vector.getValue<uint128_t>(pos)));
    case LogicalTypeID::FLOAT:
        return makeNumberOperand(
            realToNumberText(static_cast<double>(vector.getValue<float>(pos)), errorPrefix));
    case LogicalTypeID::DOUBLE:
        return makeNumberOperand(realToNumberText(vector.getValue<double>(pos), errorPrefix));
    case LogicalTypeID::LIST:
    case LogicalTypeID::ARRAY: {
        // jsonify serializes a typed list losslessly into compact JSON text;
        // re-entering the JSON path keeps one array classification for both.
        auto serialized = jsonToString(jsonify(vector, pos));
        if (serialized.empty()) {
            // jsonToString only yields "" when serialization failed; never
            // let that silently become an empty string operand.
            throw RuntimeException(
                std::format("{}: failed to serialize LIST operand", errorPrefix));
        }
        return classifyJsonText(serialized, errorPrefix);
    }
    default:
        throw RuntimeException(std::format("{}: unsupported operand type {}", errorPrefix,
            LogicalTypeUtils::toString(vector.dataType.getLogicalTypeID())));
    }
}

int compareOrderedOperands(const GqlOrderedOperand& lhs, const GqlOrderedOperand& rhs,
    const char* errorPrefix) {
    if (lhs.rank != rhs.rank) {
        return lhs.rank < rhs.rank ? -1 : 1;
    }
    switch (lhs.rank) {
    case 0:
        return 0;
    case 1:
        return lhs.boolValue == rhs.boolValue ? 0 : (lhs.boolValue ? 1 : -1);
    case 2:
        try {
            return compareJsonValues(lhs.jsonValue, rhs.jsonValue);
        } catch (const RuntimeException&) {
            // compareJsonValues only rejects distinct objects nested in
            // arrays; re-issue under this bridge's prefix.
            throw RuntimeException(std::format(
                "{}: JSON objects do not have a well-defined total order", errorPrefix));
        }
    case 3:
        return compareOperandTexts(lhs.text, rhs.text);
    case 4:
        return compareDecimalTexts(lhs.text, rhs.text, errorPrefix);
    default:
        if (jsonDeepEquals(lhs.jsonValue, rhs.jsonValue)) {
            return 0;
        }
        throw RuntimeException(
            std::format("{}: JSON objects do not have a well-defined total order", errorPrefix));
    }
}

// -----------------------------------------------------------------------------
// _GQL_SORTKEY encoding
// -----------------------------------------------------------------------------
// Byte-comparable key per class (engine STRING order is memcmp):
//   null   `a`
//   bool   `b` + `0`/`1`
//   array  `c` + elements + 0x00
//   string `d` + text (0x00-terminated and 0x00-escaped inside arrays)
//   number `e` + sign(`0` neg / `1` zero / `2` pos) + 20-digit biased int64
//          exponent + canonical mantissa digits + terminator (0x00 pos /
//          0xFF neg — see below)
// Objects have no total order and fail loudly.
//
// Number payload note: no width caps — the key accepts exactly the numbers
// the order predicates accept (one shared decimal path), so ORDER BY can
// never throw on a value WHERE compares fine. The mantissa is the canonical
// digit string (no trailing zeros) with a self-delimiting terminator chosen
// so byte order equals numeric order: positive numbers need "shorter prefix
// is smaller" (a terminator below every digit, 0x00), while negative numbers
// compare by 9's complement where a shorter prefix must sort *larger* (the
// terminator must sit above every complemented digit, 0xFF).
//
// Array framing note: the task brief specified an 8-hex-digit element length
// prefix, but a length field decides byte comparison *before* the element's
// rank/contents, so it silently mis-orders exactly the values this key exists
// to order — e.g. ["b"] (len 2) would sort below ["abc"] (len 4) although
// "abc" < "b". Elements are therefore self-delimiting instead (FoundationDB
// tuple style): variable-length payloads end with a terminator and string
// payloads escape 0x00 as 0x00 0xFF, which keeps byte order equal to element
// order and makes a shorter element sequence a byte prefix (so it sorts
// first).

void appendEscapedText(const std::string& text, std::string& out) {
    for (auto c : text) {
        out += c;
        if (c == '\0') {
            out += static_cast<char>(0xFF);
        }
    }
}

void appendNumberSortKey(std::string_view text, const char* errorPrefix, std::string& out) {
    auto parts = normalizeDecimalText(text, errorPrefix);
    out += 'e';
    if (parts.sign == 0) {
        // -0.0 and 0 collapse into one canonical zero key ('1' is unique to
        // zero, so the key self-delimits against the other sign classes).
        out += '1';
        return;
    }
    // Biased into [0, 2^64-1] over the full int64 exponent range; a fixed
    // 20-digit field keeps byte order equal to exponent order (no length
    // prefix to interfere).
    const uint64_t biasedExponent = static_cast<uint64_t>(parts.exponent) + (uint64_t{1} << 63);
    std::string exponentText = std::format("{:020d}", biasedExponent);
    std::string mantissa = parts.digits;
    if (parts.sign > 0) {
        out += '2';
        out += exponentText;
        out += mantissa;
        // Terminator below every digit: a shorter mantissa (smaller value at
        // equal exponent) byte-sorts first.
        out += '\0';
    } else {
        out += '0';
        // Negative numbers reverse both fields digit-wise (9's complement), so
        // a larger magnitude byte-sorts smaller within the negative class.
        for (auto& c : exponentText) {
            c = static_cast<char>('9' - (c - '0'));
        }
        for (auto& c : mantissa) {
            c = static_cast<char>('9' - (c - '0'));
        }
        out += exponentText;
        out += mantissa;
        // Terminator above every complemented digit: a shorter mantissa
        // (larger value, i.e. less negative) byte-sorts after a longer one.
        out += static_cast<char>(0xFF);
    }
}

void appendSortKey(const GqlOrderedOperand& operand, bool asElement, const char* errorPrefix,
    std::string& out) {
    switch (operand.rank) {
    case 0:
        out += 'a';
        return;
    case 1:
        out += 'b';
        out += operand.boolValue ? '1' : '0';
        return;
    case 2: {
        out += 'c';
        auto size = yyjson_arr_size(operand.jsonValue);
        for (size_t i = 0; i < size; ++i) {
            auto element = classifyJsonValue(yyjson_arr_get(operand.jsonValue, i), errorPrefix);
            appendSortKey(element, true /* asElement */, errorPrefix, out);
        }
        out += '\0';
        return;
    }
    case 3:
        out += 'd';
        if (asElement) {
            appendEscapedText(operand.text, out);
            out += '\0';
        } else {
            out += operand.text;
        }
        return;
    case 4:
        appendNumberSortKey(operand.text, errorPrefix, out);
        return;
    default:
        throw RuntimeException(
            std::format("{}: JSON objects do not have a well-defined total order", errorPrefix));
    }
}

// -----------------------------------------------------------------------------
// Execution
// -----------------------------------------------------------------------------

enum class OrderComparison : uint8_t { LT, LE, GT, GE, EQ, NE };

// SQL NULL on either side yields NULL — engine three-valued logic consumes it
// (WHERE drops the row, NOT(NULL) stays NULL), so nothing is classified and no
// value decides the result.
template<OrderComparison OP>
void orderPredicateExecFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    DASSERT(parameters.size() == 2);
    const auto& lhs = *parameters[0];
    const auto& rhs = *parameters[1];
    const bool lhsFlat = lhs.state->isFlat();
    const bool rhsFlat = rhs.state->isFlat();
    // Same position convention as BinaryFunctionExecutor: flat operands are
    // read at their single selected position and a flat result is one value.
    sel_t numSelectedValues = parameterSelVectors[0]->getSelSize();
    if (lhsFlat) {
        numSelectedValues = rhsFlat ? 1 : parameterSelVectors[1]->getSelSize();
    }
    for (sel_t i = 0; i < numSelectedValues; ++i) {
        auto lhsPos = (*parameterSelVectors[0])[lhsFlat ? 0 : i];
        auto rhsPos = (*parameterSelVectors[1])[rhsFlat ? 0 : i];
        auto resultPos = (*resultSelVector)[lhsFlat && rhsFlat ? 0 : i];
        if (lhs.isNull(lhsPos) || rhs.isNull(rhsPos)) {
            result.setNull(resultPos, true);
            continue;
        }
        auto lhsOperand = classifyOperand(lhs, lhsPos, ORDER_PREDICATE_PREFIX);
        auto rhsOperand = classifyOperand(rhs, rhsPos, ORDER_PREDICATE_PREFIX);
        auto cmp = compareOrderedOperands(lhsOperand, rhsOperand, ORDER_PREDICATE_PREFIX);
        bool value = false;
        switch (OP) {
        case OrderComparison::LT:
            value = cmp < 0;
            break;
        case OrderComparison::LE:
            value = cmp <= 0;
            break;
        case OrderComparison::GT:
            value = cmp > 0;
            break;
        case OrderComparison::GE:
            value = cmp >= 0;
            break;
        case OrderComparison::EQ:
            value = cmp == 0;
            break;
        case OrderComparison::NE:
            value = cmp != 0;
            break;
        }
        result.setNull(resultPos, false);
        result.setValue<bool>(resultPos, value);
    }
}

void sortKeyExecFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    DASSERT(parameters.size() == 1);
    result.resetAuxiliaryBuffer();
    const auto& input = *parameters[0];
    const bool inputFlat = input.state->isFlat();
    for (sel_t i = 0; i < resultSelVector->getSelSize(); ++i) {
        auto inputPos = (*parameterSelVectors[0])[inputFlat ? 0 : i];
        auto resultPos = (*resultSelVector)[i];
        // SQL NULL stays NULL; the engine sorts NULL keys last, unchanged.
        if (input.isNull(inputPos)) {
            result.setNull(resultPos, true);
            continue;
        }
        std::string key;
        auto operand = classifyOperand(input, inputPos, SORTKEY_PREFIX);
        appendSortKey(operand, false /* asElement */, SORTKEY_PREFIX, key);
        result.setNull(resultPos, false);
        StringVector::addString(&result, resultPos, key);
    }
}

// -----------------------------------------------------------------------------
// Registration helpers
// -----------------------------------------------------------------------------

// ANY parameters match every argument without inserting a cast (the binder
// leaves ANY targets alone), so the exec sees each argument's own logical
// type — the same pattern as the _GQL_MAX/_GQL_MIN ANY overloads.
template<OrderComparison OP>
function_set buildOrderPredicateFunctionSet(const std::string& funcName) {
    function_set result;
    result.push_back(std::make_unique<ScalarFunction>(funcName,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY, LogicalTypeID::ANY}, LogicalTypeID::BOOL,
        orderPredicateExecFunc<OP>));
    return result;
}

function_set buildSortKeyFunctionSet(const std::string& funcName) {
    function_set result;
    result.push_back(std::make_unique<ScalarFunction>(funcName,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::STRING, sortKeyExecFunc));
    return result;
}

} // namespace

function_set GqlLtFunction::getFunctionSet() {
    return buildOrderPredicateFunctionSet<OrderComparison::LT>(name);
}

function_set GqlLeFunction::getFunctionSet() {
    return buildOrderPredicateFunctionSet<OrderComparison::LE>(name);
}

function_set GqlGtFunction::getFunctionSet() {
    return buildOrderPredicateFunctionSet<OrderComparison::GT>(name);
}

function_set GqlGeFunction::getFunctionSet() {
    return buildOrderPredicateFunctionSet<OrderComparison::GE>(name);
}

function_set GqlEqFunction::getFunctionSet() {
    return buildOrderPredicateFunctionSet<OrderComparison::EQ>(name);
}

function_set GqlNeFunction::getFunctionSet() {
    return buildOrderPredicateFunctionSet<OrderComparison::NE>(name);
}

function_set GqlSortKeyFunction::getFunctionSet() {
    return buildSortKeyFunctionSet(name);
}

// =============================================================================
// _GQL_IS_SIMPLE(ANY) -> BOOL
// =============================================================================
// ISO GQL SIMPLE path predicate over a RECURSIVE_REL path value: true iff
// every node of the path is distinct, with the single exemption that the
// first and last node may be the same (closed cycle). This is strictly
// weaker than IS_ACYCLIC (which forbids first=last) and strictly stronger
// than the engine's `*ACYCLIC` slot prefilter (which only distincts
// intermediate nodes and lets endpoints collide with them) — hence the
// dedicated predicate the GQL SIMPLE path mode wraps its path variable in.
// Node identities are read with the same layout as UnaryPathExecutor's
// IS_TRAIL/IS_ACYCLIC (src/include/function/path/path_function_executor.h):
// NODES = LIST of NODE structs, internalID at field 0.

namespace {

// Pairwise-distinct check over the first `count` node IDs of one path's node
// list. `count` already excludes a duplicated last element when first==last
// (see execFunc), so every node of the path is checked exactly once.
bool isDistinctPrefix(common::ValueVector* idsVector, common::offset_t base, uint32_t count,
    std::unordered_set<common::internalID_t, InternalIDHasher>& seen) {
    seen.clear();
    for (uint32_t i = 0; i < count; ++i) {
        auto& id = idsVector->getValue<common::internalID_t>(base + i);
        if (!seen.insert(id).second) {
            return false;
        }
    }
    return true;
}

// Runtime shape guard for the path layout this predicate reads. The DASSERTs
// alone are not enough: this is a public ANY-typed scalar, so an untrusted
// query (`RETURN _GQL_IS_SIMPLE(1)`) can reach the field-vector path with a
// non-path value, where StructVector::getFieldVector is a reinterpret_cast in
// release builds. Validate the layout and throw — never reinterpret.
void ensurePathNodeIDLayout(const LogicalType& type) {
    constexpr const char* kFn = "_GQL_IS_SIMPLE";
    if (type.getPhysicalType() != PhysicalTypeID::STRUCT) {
        throw RuntimeException(
            std::format("{}: expected a path value, got {}", kFn, type.toString()));
    }
    if (!StructType::hasField(type, InternalKeyword::NODES)) {
        throw RuntimeException(std::format("{}: expected a path value with a {} field, got {}",
            kFn, std::string(InternalKeyword::NODES), type.toString()));
    }
    const auto& nodesType = StructType::getFieldType(type, InternalKeyword::NODES);
    if (nodesType.getLogicalTypeID() != LogicalTypeID::LIST) {
        throw RuntimeException(std::format("{}: path {} field must be a LIST, got {}", kFn,
            std::string(InternalKeyword::NODES), nodesType.toString()));
    }
    const auto& nodeType = ListType::getChildType(nodesType);
    if (nodeType.getPhysicalType() != PhysicalTypeID::STRUCT) {
        throw RuntimeException(std::format("{}: path {} elements must be NODE structs, got {}",
            kFn, std::string(InternalKeyword::NODES), nodeType.toString()));
    }
    if (!StructType::hasField(nodeType, InternalKeyword::ID)) {
        throw RuntimeException(std::format("{}: path NODE elements must have an {} field, got {}",
            kFn, std::string(InternalKeyword::ID), nodeType.toString()));
    }
    // Layout contract shared with UnaryPathExecutor::executeNodeIDs
    // (src/include/function/path/path_function_executor.h): internalID at
    // field 0 of each NODE struct.
    if (0 != StructType::getFieldIdx(nodeType, InternalKeyword::ID)) {
        throw RuntimeException(std::format(
            "{}: path NODE {} field must be at layout position 0", kFn, std::string(InternalKeyword::ID)));
    }
    const auto& idType = StructType::getFieldType(nodeType, InternalKeyword::ID);
    if (idType.getLogicalTypeID() != LogicalTypeID::INTERNAL_ID) {
        throw RuntimeException(std::format("{}: path NODE {} field must be INTERNAL_ID, got {}",
            kFn, std::string(InternalKeyword::ID), idType.toString()));
    }
}

} // namespace

void GqlIsSimpleFunction::execFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    DASSERT(parameters.size() == 1);
    auto& input = *parameters[0];
    // Path struct: NODES (LIST of NODE); internalID at field 0 of each NODE —
    // the exact layout UnaryPathExecutor::executeNodeIDs reads. Validated at
    // runtime (public ANY-typed scalar), looked up by field name below.
    ensurePathNodeIDLayout(input.dataType);
    auto nodesIdx = StructType::getFieldIdx(input.dataType, InternalKeyword::NODES);
    auto nodesVector = StructVector::getFieldVector(&input, nodesIdx).get();
    auto listDataVector = ListVector::getDataVector(nodesVector);
    auto idsVector = StructVector::getFieldVector(listDataVector, 0).get();
    auto& inputSel = *parameterSelVectors[0];
    const auto inputSelSize = inputSel.getSelSize();
    // Pair result rows with input rows. A single-row input may broadcast over
    // an unflat result; any other size mismatch is a dispatch contract
    // violation and must fail loudly rather than read the sel vector OOB.
    if (inputSelSize != 1 && inputSelSize != resultSelVector->getSelSize()) {
        throw RuntimeException(std::format(
            "_GQL_IS_SIMPLE: input/result selection size mismatch ({} vs {})", inputSelSize,
            resultSelVector->getSelSize()));
    }
    std::unordered_set<common::internalID_t, InternalIDHasher> seen;
    for (auto i = 0u; i < resultSelVector->getSelSize(); ++i) {
        auto inputPos = inputSel[inputSelSize == 1 ? 0 : i];
        auto resultPos = (*resultSelVector)[i];
        if (input.isNull(inputPos)) {
            result.setNull(resultPos, true);
            continue;
        }
        result.setNull(resultPos, false);
        auto& listEntry = nodesVector->getValue<common::list_entry_t>(inputPos);
        const uint32_t n = static_cast<uint32_t>(listEntry.size);
        bool simple;
        if (n <= 1) {
            simple = true; // empty / single-node path: nothing can repeat
        } else {
            auto& first = idsVector->getValue<common::internalID_t>(listEntry.offset);
            auto& last = idsVector->getValue<common::internalID_t>(listEntry.offset + n - 1);
            // Closed cycle: the only allowed repeat is first==last, so checking
            // the leading n-1 IDs (first included, duplicate last excluded)
            // pairwise-distinct covers every node exactly once.
            uint32_t count = (first == last) ? n - 1 : n;
            simple = isDistinctPrefix(idsVector, listEntry.offset, count, seen);
        }
        result.setValue<bool>(resultPos, simple);
    }
}

function_set GqlIsSimpleFunction::getFunctionSet() {
    function_set result;
    result.push_back(std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::BOOL, execFunc));
    return result;
}

// =============================================================================
// _GQL_LIST_CHECKED(ANY...) -> LIST
// =============================================================================
// Q5-3 bind-time heterogeneous-list guard for typed graphs. LadybugDB's
// list_creation bindFunc silently homogenizes mixed element types (STRING is
// the universal sink), erasing INT/DOUBLE distinctions and stringifying
// losers — a silent wrong answer for GQL, which preserves per-element types.
// The translation layer wraps every typed-graph list literal that contains a
// non-literal element in this call (pure literals stay on the engine's own
// list binding: scanValueShapes already rejected heterogeneous pure literals
// at translation time; ANY graphs and unresolvable kinds are never wrapped —
// their JSON property columns are dynamically typed and a static class
// comparison there would reject legal dynamic queries).
//
// bindFunc sees the engine-derived argument types BEFORE the post-bind cast
// (bind_function_expression.cpp runs bindFunc first, then casts to
// bindData->paramTypes), so the class comparison runs at exactly the point
// where the engine would otherwise commit to a homogenized element type.
// Null literals and untyped placeholders (ANY) are unjudgeable and skipped;
// every other pair must land in the same class as scanValueShapes'
// partition: one int family, one double family, and string/bool/list/map
// each their own (INT vs DOUBLE is heterogeneous — same rule the static
// guard enforces for literals). A mismatch throws loudly; agreement falls
// through to list_creation's own combination so the pinned LIST(<T>) element
// type and the per-argument casts match native list binding byte-for-byte.

// Type class for one judgeable argument type (matches literalTypeClass's
// partition in gql_transformer.cpp; exotic types keep their own identity so
// identical exotic lists pass while cross-type mixes throw).
static std::string listCheckedTypeClass(const common::LogicalType& type) {
    switch (type.getLogicalTypeID()) {
    case common::LogicalTypeID::INT8:
    case common::LogicalTypeID::INT16:
    case common::LogicalTypeID::INT32:
    case common::LogicalTypeID::INT64:
    case common::LogicalTypeID::INT128:
    case common::LogicalTypeID::UINT8:
    case common::LogicalTypeID::UINT16:
    case common::LogicalTypeID::UINT32:
    case common::LogicalTypeID::UINT64:
    case common::LogicalTypeID::UINT128:
    case common::LogicalTypeID::SERIAL:
        return "int";
    case common::LogicalTypeID::FLOAT:
    case common::LogicalTypeID::DOUBLE:
    case common::LogicalTypeID::DECIMAL:
        return "double";
    case common::LogicalTypeID::STRING:
        return "string";
    case common::LogicalTypeID::BOOL:
        return "bool";
    case common::LogicalTypeID::LIST:
    case common::LogicalTypeID::ARRAY:
        return "list";
    case common::LogicalTypeID::MAP:
        return "map";
    default:
        return "type:" +
               common::LogicalTypeUtils::toString(type.getLogicalTypeID());
    }
}

static std::unique_ptr<FunctionBindData> bindListChecked(
    const ScalarBindFuncInput& input) {
    // Class gate: judgeable arguments must agree; unjudgeable ones (null
    // literal, ANY placeholder) never veto. The loud throw is the whole
    // point of this function — never fall through to silent homogenization.
    std::string seenClass;
    for (auto& arg : input.arguments) {
        const auto& type = arg->getDataType();
        if (type.getLogicalTypeID() == common::LogicalTypeID::ANY ||
            binder::ExpressionUtil::isNullLiteral(*arg)) {
            continue;
        }
        std::string cls = listCheckedTypeClass(type);
        if (seenClass.empty()) {
            seenClass = std::move(cls);
        } else if (cls != seenClass) {
            throw common::RuntimeException{
                "GQL feature not supported: heterogeneous list element types"};
        }
    }
    // Same combination as ListCreationFunction::bindFunc: within one class
    // this widens (INT32 + INT64 -> INT64) or keeps the type as-is; the
    // cross-class STRING sink is unreachable because the gate above already
    // threw. Result: LIST(pinned) with one pinned cast per argument, so
    // exec sees uniform physical element data.
    LogicalType combinedType(LogicalTypeID::ANY);
    std::unordered_set<LogicalTypeID> distinctTypes;
    for (auto& arg : input.arguments) {
        auto typeID = arg->getDataType().getLogicalTypeID();
        if (typeID != LogicalTypeID::ANY) {
            distinctTypes.insert(typeID);
        }
    }
    const bool mixedConcreteTypes = distinctTypes.size() > 1;
    if (mixedConcreteTypes) {
        binder::ExpressionUtil::tryCombineDataType(input.arguments, combinedType);
        if (combinedType.getLogicalTypeID() == LogicalTypeID::ANY) {
            if (distinctTypes.contains(LogicalTypeID::STRING)) {
                combinedType = LogicalType::STRING();
            } else {
                for (auto& arg : input.arguments) {
                    if (arg->getDataType().getLogicalTypeID() != LogicalTypeID::ANY) {
                        combinedType = arg->getDataType().copy();
                        break;
                    }
                }
            }
        }
    } else {
        binder::ExpressionUtil::tryCombineDataType(input.arguments, combinedType);
        if (combinedType.getLogicalTypeID() == LogicalTypeID::ANY) {
            combinedType = LogicalType::INT64();
        }
    }
    if (combinedType.containsAny()) {
        combinedType = LogicalType::JSON();
    }
    auto resultType = LogicalType::LIST(combinedType.copy());
    auto bindData = std::make_unique<FunctionBindData>(std::move(resultType));
    for (auto& _ : input.arguments) {
        (void)_; 
        bindData->paramTypes.push_back(combinedType.copy());
    }
    return bindData;
}

// Assembles the N argument vectors into one list row — the exact construction
// of ListCreationFunction::execFunc (resetAuxiliaryBuffer, addList, per-slot
// copyFromVectorData with flat/unflat position handling).
void GqlListCheckedFunction::execFunc(
    const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    result.resetAuxiliaryBuffer();
    for (auto selectedPos = 0u; selectedPos < resultSelVector->getSelSize();
         ++selectedPos) {
        auto pos = (*resultSelVector)[selectedPos];
        auto resultEntry = ListVector::addList(&result, parameters.size());
        result.setValue(pos, resultEntry);
        auto resultDataVector = ListVector::getDataVector(&result);
        auto resultPos = resultEntry.offset;
        for (auto i = 0u; i < parameters.size(); i++) {
            const auto& parameter = parameters[i];
            const auto& parameterSelVector = *parameterSelVectors[i];
            auto paramPos = parameter->state->isFlat() ? parameterSelVector[0] : pos;
            resultDataVector->copyFromVectorData(resultPos++, parameter.get(), paramPos);
        }
    }
}

function_set GqlListCheckedFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::LIST, execFunc);
    function->bindFunc = bindListChecked;
    function->isVarLength = true;
    result.push_back(std::move(function));
    return result;
}

} // namespace gql_extension
} // namespace lbug
