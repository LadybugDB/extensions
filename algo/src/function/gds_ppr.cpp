// GDS_PPR — personalized PageRank backed by icebug (NetworKit's PageRank::forSources).
// Random walk with restart: teleportation is restricted to the caller-supplied source nodes
// (uniform over the set), so scores measure standing *relative to those anchors* rather than
// globally. This is the trust-propagation primitive: run it from a viewpoint's attested keys
// and every node inherits a confidence score relative to that viewpoint.
//
// CALL GDS_PPR('G', [rowid, ...]) — sources are node rowids (offsets), the same identity the
// materialized CSR is built on. Optional params: dampingFactor, tolerance (as GDS_PAGE_RANK).
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/value/nested.h"
#include "function/algo_function.h"
#include "function/config/page_rank_config.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/gds_csr_bridge.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <arrow/api.h>
#include <networkit/centrality/PageRank.hpp>
#include <networkit/graph/GraphR.hpp>

using namespace lbug::processor;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

struct GDSPprOptionalParams final : public function::OptionalParams {
    OptionalParam<DampingFactor> dampingFactor;
    OptionalParam<Tolerance> tolerance;

    explicit GDSPprOptionalParams(const expression_vector& optionalParams) {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == DampingFactor::NAME) {
                dampingFactor = function::OptionalParam<DampingFactor>(optionalParam);
            } else if (paramName == Tolerance::NAME) {
                tolerance = function::OptionalParam<Tolerance>(optionalParam);
            } else {
                throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    GDSPprOptionalParams(OptionalParam<DampingFactor> dampingFactor,
        OptionalParam<Tolerance> tolerance)
        : dampingFactor{std::move(dampingFactor)}, tolerance{std::move(tolerance)} {}

    void evaluateParams(main::ClientContext* context) override {
        dampingFactor.evaluateParam(context);
        tolerance.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSPprOptionalParams>(dampingFactor, tolerance);
    }
};

struct GDSPprBindData final : public GDSBindData {
    // Projected graph name, for looking up the entry's materialized arrow CSR at run time.
    std::string graphName;
    // Teleportation targets (node rowids), validated non-empty and non-negative at bind;
    // range-checked against the projected node count at run time.
    std::vector<uint64_t> sources;

    GDSPprBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector output, std::unique_ptr<GDSPprOptionalParams> optionalParams,
        std::string graphName, std::vector<uint64_t> sources)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          graphName{std::move(graphName)}, sources{std::move(sources)} {
        this->optionalParams = std::move(optionalParams);
    }

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GDSPprBindData>(*this);
    }
};

// Emits (node, rank) rows, reading the icebug PPR scores by node offset.
class GDSPprResultVertexCompute : public GDSResultVertexCompute {
public:
    GDSPprResultVertexCompute(storage::MemoryManager* mm, GDSFuncSharedState* sharedState,
        const std::vector<double>& scores)
        : GDSResultVertexCompute{mm, sharedState}, scores{scores} {
        nodeIDVector = createVector(LogicalType::INTERNAL_ID());
        rankVector = createVector(LogicalType::DOUBLE());
    }

    void beginOnTableInternal(table_id_t) override {}

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            nodeIDVector->setValue<nodeID_t>(0, nodeID_t{i, tableID});
            rankVector->setValue<double>(0, i < scores.size() ? scores[i] : 0.0);
            localFT->append(vectors);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<GDSPprResultVertexCompute>(mm, sharedState, scores);
    }

private:
    const std::vector<double>& scores;
    std::unique_ptr<ValueVector> nodeIDVector;
    std::unique_ptr<ValueVector> rankVector;
};

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
    // MVP: single node table (the common case; multi-table is a follow-up).
    if (maxOffsetMap.size() != 1) {
        throw BinderException{"GDS_PPR currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffsetMap.begin()->first;
    const auto numNodes = maxOffsetMap.begin()->second;
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GDSPprBindData>();
    auto& config = bindData->optionalParams->constCast<GDSPprOptionalParams>();
    for (const auto source : bindData->sources) {
        if (source >= numNodes) {
            throw BinderException{"GDS_PPR source node offset " + std::to_string(source) +
                                  " is out of range: the projected graph has " +
                                  std::to_string(numNodes) + " nodes."};
        }
    }

    // 1. Undirected CSR — zero-copy from the projected graph's materialized arrow CSR when
    // available, scan fallback otherwise (see gds_csr_bridge.cpp).
    auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID, numNodes, mm);

    // 2. icebug: zero-copy GraphR, then k-source personalized PageRank (uniform teleportation
    // over the sources; memory-efficient — no n-sized personalization vector).
    NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
    const std::vector<NetworKit::node> sources{bindData->sources.begin(), bindData->sources.end()};
    auto pr = NetworKit::PageRank::forSources(g, sources, config.dampingFactor.getParamVal(),
        config.tolerance.getParamVal());
    pr.run();
    const std::vector<double>& scores = pr.scores();

    // 3. Stream scores back through the GDS result pipeline.
    auto outputVC = std::make_unique<GDSPprResultVertexCompute>(mm, sharedState, scores);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *outputVC);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static constexpr char RANK_COLUMN_NAME[] = "rank";

static std::vector<uint64_t> extractSources(const Value& value) {
    value.validateType(LogicalTypeID::LIST);
    std::vector<uint64_t> sources;
    sources.reserve(NestedVal::getChildrenSize(&value));
    for (auto i = 0u; i < NestedVal::getChildrenSize(&value); ++i) {
        const auto* child = NestedVal::getChildVal(&value, i);
        child->validateType(LogicalTypeID::INT64);
        const auto source = child->getValue<int64_t>();
        if (source < 0) {
            throw BinderException{"GDS_PPR source node offsets must be non-negative."};
        }
        sources.push_back(static_cast<uint64_t>(source));
    }
    if (sources.empty()) {
        throw BinderException{"GDS_PPR requires at least one source node."};
    }
    return sources;
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto sources = extractSources(input->getValue(1));
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(RANK_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<GDSPprBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput},
        std::make_unique<GDSPprOptionalParams>(input->optionalParamsLegacy), std::move(graphName),
        std::move(sources));
}

function_set GDSPprFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GDSPprFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY, LogicalTypeID::ANY});
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
