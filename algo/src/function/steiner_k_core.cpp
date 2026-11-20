// Single-seed k-core community search backed by icebug 13.6
// NetworKit::SteinerKCore::communities().
//
// CALL GDS_STEINER_K_CORE('G', [seeds]) returns one row per community member:
// (node, community_id, coreness). Single seed set only: every member shares
// community_id 0 and the community's coreness k — the connected component of
// the k-core containing the seeds, for the largest such k. Batched multi-set
// queries (one community per seed set) are future work; they need an SCD-shape
// builder that does not fit the per-node make*Func builders in
// gds_generic_algo.h.
#include "function/steiner_k_core.h"

#include <set>
#include <vector>

#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/types/value/nested.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/gds_csr_bridge.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"

#if defined(ICEBUG_ENABLED)
#include <networkit/graph/GraphR.hpp>
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
    std::string graphName;

    SteinerKCoreBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector output, std::vector<int64_t> seeds, std::string graphName)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          seeds{std::move(seeds)}, graphName{std::move(graphName)} {}

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

// Emits one row per community member; non-members (and masked-out nodes) are skipped.
class SteinerKCoreResultVertexCompute final : public GDSResultVertexCompute {
public:
    SteinerKCoreResultVertexCompute(MemoryManager* mm, GDSFuncSharedState* sharedState,
        const std::vector<char>& isMember, int64_t coreness)
        : GDSResultVertexCompute{mm, sharedState}, isMember{isMember}, coreness{coreness} {
        nodeIDVector = createVector(LogicalType::INTERNAL_ID());
        communityIDVector = createVector(LogicalType::INT64());
        corenessVector = createVector(LogicalType::INT64());
    }

    void beginOnTableInternal(table_id_t) override {}

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (i >= isMember.size() || !isMember[i] || skip(i)) {
                continue;
            }
            nodeIDVector->setValue<nodeID_t>(0, nodeID_t{i, tableID});
            // Single seed set: one community, id 0; coreness k shared by all members.
            communityIDVector->setValue<int64_t>(0, 0);
            corenessVector->setValue<int64_t>(0, coreness);
            localFT->append(vectors);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<SteinerKCoreResultVertexCompute>(mm, sharedState, isMember,
            coreness);
    }

private:
    const std::vector<char>& isMember;
    int64_t coreness;
    std::unique_ptr<ValueVector> nodeIDVector;
    std::unique_ptr<ValueVector> communityIDVector;
    std::unique_ptr<ValueVector> corenessVector;
};

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
    if (maxOffsetMap.size() != 1) {
        throw BinderException{"GDS functions currently support single-node-table "
                              "graphs only."};
    }
    const auto tableID = maxOffsetMap.begin()->first;
    const auto numNodes = maxOffsetMap.begin()->second;
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<SteinerKCoreBindData>();
    for (const auto seed : bindData->seeds) {
        if (static_cast<common::offset_t>(seed) >= numNodes) {
            throw BinderException{std::string{SteinerKCoreFunction::name} + " seed node offset " +
                                  std::to_string(seed) +
                                  " is out of range: the projected graph has " +
                                  std::to_string(numNodes) + " nodes."};
        }
    }
    std::set<NetworKit::node> seedSet;
    for (const auto seed : bindData->seeds) {
        seedSet.insert(static_cast<NetworKit::node>(seed));
    }
    auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID, numNodes, mm);
    NetworKit::GraphR rawGraph(static_cast<NetworKit::count>(numNodes), /*directed=*/false,
        csr.indices, csr.indptr);
    NetworKit::Graph networKitGraph(rawGraph);
    NetworKit::SteinerKCore steiner(networKitGraph);
    auto result = steiner.communities({seedSet});
    const auto coreness = static_cast<int64_t>(result.coreness[0]);
    std::vector<char> isMember(numNodes, 0);
    for (const auto v : result.communities[0]) {
        if (v < static_cast<NetworKit::node>(numNodes)) {
            isMember[v] = 1;
        }
    }
    auto outputVC =
        std::make_unique<SteinerKCoreResultVertexCompute>(mm, sharedState, isMember, coreness);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *outputVC);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
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
        expression_vector{nodeOutput}, std::move(seeds), graphName);
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
