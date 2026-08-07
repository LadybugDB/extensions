// GDS_PAGE_RANK — PageRank backed by icebug (a NetworKit fork with zero-copy Arrow CSR ingest).
// Same CALL surface as the hand-rolled PAGE_RANK, but the compute is delegated to libnetworkit:
// we materialize the projected graph's adjacency as CSR, build a NetworKit::GraphR over Arrow
// buffers, run NetworKit::PageRank, and stream the scores back through the GDS result pipeline.
//
// Part of the icebug bridge (adsharma-invited): the `algo` extension keeps its existing algos for
// one release cycle; the icebug-backed ones live alongside under GDS_* names.
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/in_mem_graph.h"
#include "common/string_utils.h"
#include "function/algo_function.h"
#include "function/config/max_iterations_config.h"
#include "function/config/page_rank_config.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
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

// Reuse the PAGE_RANK optional params (dampingFactor, tolerance, maxIterations, normalize).
struct GDSPageRankOptionalParams final : public MaxIterationOptionalParams {
    OptionalParam<DampingFactor> dampingFactor;
    OptionalParam<Tolerance> tolerance;

    explicit GDSPageRankOptionalParams(const expression_vector& optionalParams)
        : MaxIterationOptionalParams{constructMaxIterationParam(optionalParams)} {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == DampingFactor::NAME) {
                dampingFactor = function::OptionalParam<DampingFactor>(optionalParam);
            } else if (paramName == Tolerance::NAME) {
                tolerance = function::OptionalParam<Tolerance>(optionalParam);
            } else if (paramName == MaxIterations::NAME) {
                continue;
            } else {
                throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    GDSPageRankOptionalParams(OptionalParam<MaxIterations> maxIterations,
        OptionalParam<DampingFactor> dampingFactor, OptionalParam<Tolerance> tolerance)
        : MaxIterationOptionalParams{maxIterations}, dampingFactor{std::move(dampingFactor)},
          tolerance{std::move(tolerance)} {}

    void evaluateParams(main::ClientContext* context) override {
        MaxIterationOptionalParams::evaluateParams(context);
        dampingFactor.evaluateParam(context);
        tolerance.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSPageRankOptionalParams>(maxIterations, dampingFactor, tolerance);
    }
};

struct GDSPageRankBindData final : public GDSBindData {
    GDSPageRankBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        std::shared_ptr<Expression> nodeOutput,
        std::unique_ptr<GDSPageRankOptionalParams> optionalParams)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{nodeOutput}} {
        this->optionalParams = std::move(optionalParams);
    }

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GDSPageRankBindData>(*this);
    }
};

// Emits (node, rank) rows, reading the icebug PageRank scores by node offset.
class GDSPageRankResultVertexCompute : public GDSResultVertexCompute {
public:
    GDSPageRankResultVertexCompute(storage::MemoryManager* mm, GDSFuncSharedState* sharedState,
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
        return std::make_unique<GDSPageRankResultVertexCompute>(mm, sharedState, scores);
    }

private:
    const std::vector<double>& scores;
    std::unique_ptr<ValueVector> nodeIDVector;
    std::unique_ptr<ValueVector> rankVector;
};

// Materialize the projected graph's undirected adjacency as an InMemGraph CSR (fwd + bwd),
// exactly as the Louvain path does.
static void buildCSR(table_id_t tableID, offset_t numNodes, Graph* graph, InMemGraph& inMem) {
    const auto nbrTables = graph->getRelInfos(tableID);
    const auto nbrInfo = nbrTables[0];
    const auto scanState = graph->prepareRelScan(*nbrInfo.relGroupEntry, nbrInfo.relTableID,
        nbrInfo.dstTableID, {}, false /*randomLookup*/);
    for (offset_t nodeId = 0; nodeId < numNodes; ++nodeId) {
        inMem.initNextNode();
        const nodeID_t nid = {nodeId, tableID};
        for (auto chunk : graph->scanFwd(nid, *scanState)) {
            chunk.forEach(
                [&](auto neighbors, auto, auto i) { inMem.insertNbr(neighbors[i].offset); });
        }
        for (auto chunk : graph->scanBwd(nid, *scanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                if (neighbors[i].offset != nodeId) {
                    inMem.insertNbr(neighbors[i].offset);
                }
            });
        }
    }
    inMem.initNextNode(); // trailing sentinel: csrOffsets[numNodes] == numEdges
}

static std::shared_ptr<arrow::UInt64Array> toU64(const std::function<offset_t(offset_t)>& at,
    offset_t count) {
    // Arrow's *default* pool is mimalloc-backed. When libarrow is pulled in by dlopen()ing this
    // extension, mimalloc's per-thread init races on ladybug worker threads that already existed
    // before the load, and segfaults inside _mi_thread_init (~60% of runs when GDS_PAGE_RANK runs
    // after another algo test in the same process). The system (malloc) pool has no such
    // thread-local bootstrap and is stable.
    arrow::UInt64Builder builder(arrow::system_memory_pool());
    (void)builder.Reserve(count);
    for (offset_t i = 0; i < count; ++i) {
        (void)builder.Append(static_cast<uint64_t>(at(i)));
    }
    std::shared_ptr<arrow::Array> arr;
    (void)builder.Finish(&arr);
    return std::static_pointer_cast<arrow::UInt64Array>(arr);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
    // MVP: single node table (the common case; multi-table is a follow-up).
    if (maxOffsetMap.size() != 1) {
        throw BinderException{"GDS_PAGE_RANK currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffsetMap.begin()->first;
    const auto numNodes = maxOffsetMap.begin()->second;
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GDSPageRankBindData>();
    auto& config = bindData->optionalParams->constCast<GDSPageRankOptionalParams>();

    // 1. Ladybug engine graph -> InMemGraph CSR.
    InMemGraph inMem(numNodes, mm);
    buildCSR(tableID, numNodes, graph, inMem);

    // 2. CSR -> Arrow UInt64 arrays (indptr length numNodes+1, indices length numEdges).
    auto outIndptr = toU64([&](offset_t i) { return inMem.csrOffsets[i]; }, numNodes + 1);
    auto outIndices =
        toU64([&](offset_t i) { return inMem.csrEdges[i].neighbor; }, inMem.csrEdges.size());

    // 3. icebug: zero-copy GraphR over the Arrow CSR, then PageRank.
    NetworKit::GraphR g(numNodes, /*directed=*/false, outIndices, outIndptr);
    NetworKit::PageRank pr(g, config.dampingFactor.getParamVal(), config.tolerance.getParamVal());
    pr.run();
    const std::vector<double>& scores = pr.scores();

    // 4. Stream scores back through the GDS result pipeline.
    auto outputVC = std::make_unique<GDSPageRankResultVertexCompute>(mm, sharedState, scores);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *outputVC);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static constexpr char RANK_COLUMN_NAME[] = "rank";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(RANK_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<GDSPageRankBindData>(std::move(columns), std::move(graphEntry),
        nodeOutput, std::make_unique<GDSPageRankOptionalParams>(input->optionalParamsLegacy));
}

function_set GDSPageRankFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GDSPageRankFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY});
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
