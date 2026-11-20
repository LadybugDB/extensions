// Stub for the icebug 13.6 SteinerKCore k-core community search.
// See src/include/function/steiner_k_core.h for why this is a stub and what the
// final shape should be. Bind validates the CALL signature; tableFunc throws until
// the SCD-shape builder (graph + seed-set list in, per-member rows out) lands.
#include "function/steiner_k_core.h"

#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/types/value/nested.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"

#if defined(ICEBUG_ENABLED)
// Prove the 13.6 prebuilt / source build ships the new algorithm. No API use yet —
/// the SCD builder will call NetworKit::SteinerKCore::communities().
#include <networkit/scd/SteinerKCore.hpp>
#endif

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

namespace {

struct SteinerKCoreBindData final : public GDSBindData {
    // Seed row offsets as given in the CALL-site LIST(INT64).
    std::vector<int64_t> seeds;

    SteinerKCoreBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector output, std::vector<int64_t> seeds)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          seeds{std::move(seeds)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SteinerKCoreBindData>(*this);
    }
};

std::vector<int64_t> bindSeeds(const TableFuncBindInput* input) {
    const auto& value = input->getValue(1);
    value.validateType(LogicalTypeID::LIST);
    std::vector<int64_t> seeds;
    seeds.reserve(NestedVal::getChildrenSize(&value));
    for (auto i = 0u; i < NestedVal::getChildrenSize(&value); ++i) {
        const auto* child = NestedVal::getChildVal(&value, i);
        child->validateType(LogicalTypeID::INT64);
        const auto seed = child->getValue<int64_t>();
        if (seed < 0) {
            throw BinderException{std::string{SteinerKCoreFunction::name} +
                                  " seed node offsets must be non-negative."};
        }
        seeds.push_back(seed);
    }
    if (seeds.empty()) {
        throw BinderException{
            std::string{SteinerKCoreFunction::name} + " requires at least one seed node."};
    }
    return seeds;
}

static offset_t tableFunc(const TableFuncInput&, TableFuncOutput&) {
    // SCD-shape builder TBD: bridge the projected graph to NetworKit::GraphR, run
    // NetworKit::SteinerKCore::communities() over the bound seed sets, and stream
    // (node, community_id, coreness) rows. See steiner_k_core.h.
    throw RuntimeException{std::string{SteinerKCoreFunction::name} +
                           " is not yet implemented: the SCD-shape builder for icebug 13.6 "
                           "NetworKit::SteinerKCore is pending."};
}

static constexpr char COMMUNITY_ID_COLUMN_NAME[] = "community_id";
static constexpr char CORENESS_COLUMN_NAME[] = "coreness";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto seeds = bindSeeds(input);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(
        input->binder->createVariable(COMMUNITY_ID_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(CORENESS_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<SteinerKCoreBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, std::move(seeds));
}

} // namespace

function_set SteinerKCoreFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(SteinerKCoreFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::STRING, LogicalTypeID::LIST});
    func->parameterNames = {"graph_name", "seeds"};
    func->parameterTypes.push_back(LogicalType::STRING());
    func->parameterTypes.push_back(LogicalType::LIST(LogicalType::INT64()));
    // A bare LIST LogicalTypeID carries no child info; infer the nested type from the
    // CALL-site argument (same mechanism as GDS PPR's SOURCE_LIST extra).
    func->inferInputTypes = [](const expression_vector& params) {
        std::vector<LogicalType> types;
        types.push_back(LogicalType::STRING());
        types.push_back(params[1]->getDataType().copy());
        return types;
    };
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
