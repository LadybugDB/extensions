// GDS_NODE2VEC — node embeddings backed by icebug (NetworKit's Node2Vec).
// Produces a structural embedding per node from graph topology alone (no features, no text):
// materialize the projected graph as CSR, build a NetworKit::GraphR, run NetworKit::Node2Vec,
// and stream each node's embedding back as a LIST(FLOAT). Cosine-near embeddings = structurally
// (and, for a citation graph, conceptually) related nodes — the graph-native "semantics" primitive.
//
// MVP: fixed hyperparameters and a LIST(FLOAT) output (castable to ARRAY(FLOAT, d) for the vector
// extension's HNSW index). Configurable params + ARRAY output are follow-ups.
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "function/algo_function.h"
#include "function/gds_csr_bridge.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <arrow/api.h>
#include <networkit/embedding/Node2Vec.hpp>
#include <networkit/graph/GraphR.hpp>

using namespace lbug::processor;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

// Node2Vec hyperparameters (MVP: fixed). Modest walk length / count / dimension keep it tractable.
static constexpr double N2V_P = 1.0;         // return parameter
static constexpr double N2V_Q = 1.0;         // in-out parameter
static constexpr uint64_t N2V_WALK_LEN = 10; // walk length
static constexpr uint64_t N2V_NUM_WALKS = 5; // walks per node
static constexpr uint64_t N2V_DIM = 64;      // embedding dimension

// CSR construction lives in the shared bridge (gds_csr_bridge.cpp): zero-copy from the graph
// entry's materialized arrow CSR when available, storage-scan fallback otherwise.

struct GDSNode2VecBindData final : public GDSBindData {
    // Projected graph name, for looking up the entry's materialized arrow CSR at run time.
    std::string graphName;

    GDSNode2VecBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector output, std::string graphName)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          graphName{std::move(graphName)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GDSNode2VecBindData>(*this);
    }
};

// Emits (node, embedding) rows; embedding is a LIST(FLOAT) read from Node2Vec's features by offset.
class GDSNode2VecResultVertexCompute : public GDSResultVertexCompute {
public:
    GDSNode2VecResultVertexCompute(storage::MemoryManager* mm, GDSFuncSharedState* sharedState,
        const std::vector<std::vector<float>>& features)
        : GDSResultVertexCompute{mm, sharedState}, features{features} {
        nodeIDVector = createVector(LogicalType::INTERNAL_ID());
        embVector = createVector(LogicalType::LIST(LogicalType::FLOAT()));
    }

    void beginOnTableInternal(table_id_t) override {}

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            nodeIDVector->setValue<nodeID_t>(0, nodeID_t{i, tableID});
            const auto& emb = i < features.size() ? features[i] : empty;
            auto entry = ListVector::addList(embVector.get(), emb.size());
            embVector->setValue<list_entry_t>(0, entry);
            auto* dataVector = ListVector::getDataVector(embVector.get());
            for (auto k = 0u; k < emb.size(); ++k) {
                dataVector->setValue<float>(entry.offset + k, emb[k]);
            }
            localFT->append(vectors);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<GDSNode2VecResultVertexCompute>(mm, sharedState, features);
    }

private:
    const std::vector<std::vector<float>>& features;
    const std::vector<float> empty;
    std::unique_ptr<ValueVector> nodeIDVector;
    std::unique_ptr<ValueVector> embVector;
};

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
    if (maxOffsetMap.size() != 1) {
        throw BinderException{"GDS_NODE2VEC currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffsetMap.begin()->first;
    const auto numNodes = maxOffsetMap.begin()->second;
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GDSNode2VecBindData>();

    // 1. Undirected CSR — zero-copy from the projected graph's materialized arrow CSR when
    // available, scan fallback otherwise (see gds_csr_bridge.cpp).
    auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID, numNodes, mm);

    // 2. icebug: zero-copy GraphR, then Node2Vec.
    NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
    NetworKit::Node2Vec n2v(g, N2V_P, N2V_Q, N2V_WALK_LEN, N2V_NUM_WALKS, N2V_DIM);
    n2v.run();
    const std::vector<std::vector<float>>& features = n2v.getFeatures();

    // 3. Stream embeddings back through the GDS result pipeline.
    auto outputVC = std::make_unique<GDSNode2VecResultVertexCompute>(mm, sharedState, features);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *outputVC);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static constexpr char EMBEDDING_COLUMN_NAME[] = "embedding";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(EMBEDDING_COLUMN_NAME,
        LogicalType::LIST(LogicalType::FLOAT())));
    return std::make_unique<GDSNode2VecBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, std::move(graphName));
}

function_set GDSNode2VecFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GDSNode2VecFunction::name,
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
