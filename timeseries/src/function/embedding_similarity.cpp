// EMBEDDING_SIMILARITY — generic N-dimension feature similarity
//
// CALL embedding_similarity(
//     vec_a LIST<DOUBLE>,  -- first embedding
//     vec_b LIST<DOUBLE>,  -- second embedding
// ) RETURN similarity_score DOUBLE, dimension_count INT64
//
// Algorithm:
//   1. Compute cosine similarity: dot(a,b) / (|a| × |b|)
//   2. Compute per-dimension feature similarity: 1 - |a-b| / max(|a|,|b|,eps)
//   3. Return cosine similarity as primary score + dimension count
//
// Generalized from bitemporal's character_similarity (which was hardcoded to 4 dimensions).
// Works with embeddings of any dimension (768-dim text, 384-dim code, etc.)

#include "binder/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/timeseries_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include <cmath>
#include <string>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::processor;

namespace lbug { namespace timeseries_extension {

struct ESBD final : TableFuncBindData {
    std::vector<double> vecA;
    std::vector<double> vecB;
    ESBD(std::vector<double> a, std::vector<double> b, expression_vector c, row_idx_t n)
        : TableFuncBindData{std::move(c),n}, vecA{std::move(a)}, vecB{std::move(b)} {}
    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ESBD>(vecA,vecB,columns,numRows);
    }
};

// Cosine similarity: dot(a,b) / (|a| × |b|)
static double cosineSim(const std::vector<double>& a, const std::vector<double>& b) {
    double dot = 0, nA = 0, nB = 0;
    size_t len = std::min(a.size(), b.size());
    for (size_t i = 0; i < len; i++) {
        dot += a[i] * b[i];
        nA += a[i] * a[i];
        nB += b[i] * b[i];
    }
    if (nA < 1e-12 || nB < 1e-12) return 0.0;
    double cs = dot / (std::sqrt(nA) * std::sqrt(nB));
    if (cs > 1.0) cs = 1.0;
    if (cs < -1.0) cs = -1.0;
    return cs;
}

// Mean per-dimension feature similarity
static double meanFeatureSim(const std::vector<double>& a, const std::vector<double>& b) {
    size_t len = std::min(a.size(), b.size());
    if (len == 0) return 0.0;
    double sum = 0;
    for (size_t i = 0; i < len; i++) {
        double d = std::max(std::max(std::abs(a[i]), std::abs(b[i])), 0.001);
        sum += 1.0 - std::abs(a[i] - b[i]) / d;
    }
    return sum / len;
}

static offset_t tableFunc(const TableFuncMorsel&, const TableFuncInput& in, DataChunk& out) {
    auto bd = in.bindData->constPtrCast<ESBD>();
    double cosSim = cosineSim(bd->vecA, bd->vecB);
    double featSim = meanFeatureSim(bd->vecA, bd->vecB);
    int64_t dims = (int64_t)std::min(bd->vecA.size(), bd->vecB.size());

    auto pos = out.state->getSelVector()[0];
    out.getValueVectorMutable(0).setValue(pos, cosSim);
    out.getValueVectorMutable(1).setValue(pos, featSim);
    out.getValueVectorMutable(2).setValue(pos, dims);
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext*,
    const TableFuncBindInput* in) {
    // vec_a (param 0) — LIST<DOUBLE>
    auto av = in->getValue(0);
    uint32_t na = NestedVal::getChildrenSize(&av);
    std::vector<double> vecA; vecA.reserve(na);
    for (uint32_t j = 0; j < na; j++)
        vecA.push_back(NestedVal::getChildVal(&av, j)->getValue<double>());

    // vec_b (param 1) — LIST<DOUBLE>
    auto bv = in->getValue(1);
    uint32_t nb = NestedVal::getChildrenSize(&bv);
    std::vector<double> vecB; vecB.reserve(nb);
    for (uint32_t j = 0; j < nb; j++)
        vecB.push_back(NestedVal::getChildVal(&bv, j)->getValue<double>());

    std::vector<std::string> ns = {"cosine_similarity", "feature_similarity", "dimension_count"};
    std::vector<LogicalType> ts;
    ts.push_back(LogicalType::DOUBLE());
    ts.push_back(LogicalType::DOUBLE());
    ts.push_back(LogicalType::INT64());
    ns = TableFunction::extractYieldVariables(ns, in->yieldVariables);
    return std::make_unique<ESBD>(std::move(vecA), std::move(vecB),
                                  in->binder->createVariables(ns, ts), 1);
}

function_set EmbeddingSimilarityFunction::getFunctionSet() {
    function_set fs;
    auto f = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::ANY});
    f->inferInputTypes = [](const expression_vector&) -> std::vector<LogicalType> {
        std::vector<LogicalType> result;
        result.push_back(LogicalType::LIST(LogicalType::DOUBLE()));
        result.push_back(LogicalType::LIST(LogicalType::DOUBLE()));
        return result;
    };
    f->tableFunc = SimpleTableFunc::getTableFunc(tableFunc);
    f->bindFunc = bindFunc;
    f->initSharedStateFunc = SimpleTableFunc::initSharedState;
    f->initLocalStateFunc = TableFunction::initEmptyLocalState;
    fs.push_back(std::move(f));
    return fs;
}

}} // namespaces
