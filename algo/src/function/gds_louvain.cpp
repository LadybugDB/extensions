// GDS_LOUVAIN — Louvain community detection backed by icebug (a NetworKit fork with zero-copy
// Arrow CSR ingest). Same CALL surface as the hand-rolled LOUVAIN, but the compute is delegated
// to libnetworkit: we materialize the projected graph's adjacency as CSR, build a
// NetworKit::GraphR over Arrow buffers, run NetworKit::PLM (Parallel Louvain Method), and stream
// the resulting community assignments back through the GDS result pipeline.
//
// Part of the icebug bridge (adsharma-invited): the `algo` extension keeps its existing algos for
// one release cycle; the icebug-backed ones live alongside under GDS_* names.
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "function/algo_function.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/gds_csr_bridge.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <arrow/api.h>
#include <networkit/community/PLM.hpp>
#include <networkit/graph/GraphR.hpp>
#include <networkit/structures/Partition.hpp>

using namespace lbug::processor;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

// Emits (node, community_id) rows, reading the icebug PLM partition by node offset.
class GDSLouvainResultVertexCompute : public GDSResultVertexCompute {
public:
    GDSLouvainResultVertexCompute(storage::MemoryManager* mm, GDSFuncSharedState* sharedState,
        const NetworKit::Partition& partition)
        : GDSResultVertexCompute{mm, sharedState}, partition{partition} {
        nodeIDVector = createVector(LogicalType::INTERNAL_ID());
        communityIDVector = createVector(LogicalType::INT64());
    }

    void beginOnTableInternal(table_id_t) override {}

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            nodeIDVector->setValue<nodeID_t>(0, nodeID_t{i, tableID});
            communityIDVector->setValue<int64_t>(0,
                i < partition.numberOfElements() ? static_cast<int64_t>(partition[i]) : -1);
            localFT->append(vectors);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<GDSLouvainResultVertexCompute>(mm, sharedState, partition);
    }

private:
    const NetworKit::Partition& partition;
    std::unique_ptr<ValueVector> nodeIDVector;
    std::unique_ptr<ValueVector> communityIDVector;
};

// CSR construction lives in the shared bridge (gds_csr_bridge.cpp): zero-copy from the graph
// entry's materialized arrow CSR when available, storage-scan fallback otherwise.

struct GDSLouvainBindData final : public GDSBindData {
    // Projected graph name, for looking up the entry's materialized arrow CSR at run time.
    std::string graphName;

    GDSLouvainBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector output, std::string graphName)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          graphName{std::move(graphName)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GDSLouvainBindData>(*this);
    }
};

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
    // MVP: single node table (the common case; multi-table is a follow-up).
    if (maxOffsetMap.size() != 1) {
        throw BinderException{"GDS_LOUVAIN currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffsetMap.begin()->first;
    const auto numNodes = maxOffsetMap.begin()->second;
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GDSLouvainBindData>();

    // 1. Undirected CSR — zero-copy from the projected graph's materialized arrow CSR when
    // available, scan fallback otherwise (see gds_csr_bridge.cpp).
    auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID, numNodes, mm);

    // 2. icebug: zero-copy GraphR over the Arrow CSR, then PLM (Parallel Louvain Method).
    NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
    NetworKit::PLM plm(g);
    plm.run();
    const NetworKit::Partition& partition = plm.getPartition();

    // 4. Stream community assignments back through the GDS result pipeline.
    auto outputVC = std::make_unique<GDSLouvainResultVertexCompute>(mm, sharedState, partition);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *outputVC);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static constexpr char COMMUNITY_ID_COLUMN_NAME[] = "community_id";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(
        input->binder->createVariable(COMMUNITY_ID_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GDSLouvainBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, std::move(graphName));
}

function_set GDSLouvainFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GDSLouvainFunction::name,
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
