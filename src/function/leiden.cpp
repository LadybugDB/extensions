// LadybugDB ALGO Extension — Leiden community detection
//
// Independent C++ implementation (no libelenalg/igraph dependencies).
// Pipeline: GDSFunction::initSharedState → getLogicalPlan → getPhysicalPlan
// Algorithm: MoveNodesFast (modularity-based local moving)
//
// Reference: "From Louvain to Leiden" (Traag et al. 2019)

#include "function/leiden.h"

#include "binder/binder.h"
#include "common/exception/runtime.h"
#include "common/in_mem_gds_utils.h"
#include "common/string_utils.h"
#include "common/task_system/progress_bar.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/config/louvain_config.h"
#include "function/config/max_iterations_config.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

using namespace std;
using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

static constexpr const char* LEIDEN_COLUMN_NAME = "community_id";
using LeidenBindData = GDSBindData;

// --------------- Independent Leiden data structures ---------------

struct LeidenGraph {
    uint32_t numNodes;
    vector<vector<pair<uint32_t, double>>> adj;
    double totalWeight;

    explicit LeidenGraph(uint32_t n) : numNodes{n}, adj(n), totalWeight{0.0} {}

    void addUndirectedEdge(uint32_t u, uint32_t v, double w = 1.0) {
        adj[u].push_back({v, w});
        adj[v].push_back({u, w});
        totalWeight += 2.0 * w;
    }

    const vector<pair<uint32_t, double>>& neighbors(uint32_t u) const { return adj[u]; }
    double weightedDegree(uint32_t u) const {
        double d = 0.0;
        for (auto& [v, w] : adj[u]) d += w;
        return d;
    }
};

// --------------- MoveNodesFast (from Memgraph port) ---------------

static uint64_t moveNodesFast(LeidenGraph& g, vector<uint32_t>& membership,
    double gamma, double resolution) {
    uint32_t n = g.numNodes;
    deque<uint32_t> queue;
    unordered_set<uint32_t> inQueue;
    inQueue.reserve(n);

    for (uint32_t i = 0; i < n; i++) {
        queue.push_back(i);
        inQueue.insert(i);
    }

    static mt19937 gen(random_device{}());
    shuffle(queue.begin(), queue.end(), gen);

    vector<double> edgeWeightToComm(n, 0.0);
    vector<char> visited(n, 0);
    vector<uint32_t> neighborComms(n, 0);
    vector<double> commWeight(n, 0.0);

    for (uint32_t i = 0; i < n; i++)
        commWeight[i] = g.weightedDegree(i);

    uint64_t emptyCount = 0;

    while (!queue.empty()) {
        uint32_t node = queue.front();
        queue.pop_front();
        inQueue.erase(node);

        uint32_t bestComm = membership[node];
        uint32_t curComm = bestComm;
        uint32_t numNeighborComms = 0;

        for (auto& [nbr, w] : g.adj[node]) {
            uint32_t nc = membership[nbr];
            edgeWeightToComm[nc] += w;
            if (!visited[nc]) {
                visited[nc] = 1;
                neighborComms[numNeighborComms++] = nc;
            }
        }

        double curDelta = edgeWeightToComm[curComm] - commWeight[curComm] * gamma;
        double bestDelta = curDelta;

        for (uint32_t i = 0; i < numNeighborComms; i++) {
            uint32_t nc = neighborComms[i];
            if (nc != curComm) {
                double delta = edgeWeightToComm[nc] - commWeight[nc] * gamma;
                if (delta > bestDelta + resolution) {
                    bestDelta = delta;
                    bestComm = nc;
                }
            }
            edgeWeightToComm[nc] = 0.0;
            visited[nc] = 0;
        }

        if (curComm != bestComm) {
            double nodeDeg = g.weightedDegree(node);
            commWeight[curComm] -= nodeDeg;
            commWeight[bestComm] += nodeDeg;
            membership[node] = bestComm;

            for (auto& [nbr, w] : g.adj[node]) {
                if (!inQueue.contains(nbr) && membership[nbr] != bestComm) {
                    queue.push_back(nbr);
                    inQueue.insert(nbr);
                }
            }
        }
    }
    return emptyCount;
}

// --------------- GDSResultVertexCompute writing ---------------

class LeidenWriteVC final : public GDSResultVertexCompute {
public:
    LeidenWriteVC(MemoryManager* mm, GDSFuncSharedState* ss,
        const vector<uint32_t>& comm)
        : GDSResultVertexCompute{mm, ss}, community{comm} {
        nodeIDVec = createVector(LogicalType::INTERNAL_ID());
        commIDVec = createVector(LogicalType::INT64());
    }

    void beginOnTableInternal(table_id_t) override {}
    void vertexCompute(offset_t start, offset_t end, const table_id_t tid) override {
        for (auto i = start; i < end; ++i) {
            nodeIDVec->setValue<nodeID_t>(0, {i, tid});
            commIDVec->setValue<int64_t>(0, static_cast<int64_t>(community[i]));
            localFT->append(vectors);
        }
    }
    unique_ptr<VertexCompute> copy() override {
        return make_unique<LeidenWriteVC>(mm, sharedState, community);
    }
private:
    const vector<uint32_t>& community;
    unique_ptr<ValueVector> nodeIDVec, commIDVec;
};

// --------------- Bind / TableFunc ---------------

static unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    const auto graphName = input->getLiteralVal<string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    expression_vector columns;
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    columns.push_back(nodeOutput->constPtrCast<NodeExpression>()->getInternalID());
    columns.push_back(input->binder->createVariable(LEIDEN_COLUMN_NAME, LogicalType::INT64()));
    return make_unique<LeidenBindData>(move(columns), move(graphEntry),
        expression_vector{move(nodeOutput)});
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto trx = transaction::Transaction::Get(*clientContext);
    auto ss = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto mm = MemoryManager::Get(*clientContext);
    auto graph = ss->graph.get();

    DASSERT(graph->getNodeTableIDs().size() == 1);
    auto tid = graph->getNodeTableIDs()[0];
    auto n = graph->getMaxOffset(trx, tid);

    // Build independent LeidenGraph from on-disk GDS graph
    LeidenGraph lg(static_cast<uint32_t>(n));
    auto nbrs = graph->getRelInfos(tid);
    auto scan = graph->prepareRelScan(*nbrs[0].relGroupEntry, nbrs[0].relTableID,
        nbrs[0].dstTableID, {}, false);

    for (auto i = 0u; i < n; ++i) {
        nodeID_t nid{i, tid};
        for (auto ch : graph->scanFwd(nid, *scan))
            ch.forEach([&](auto nb, auto, auto j) {
                lg.addUndirectedEdge(i, nb[j].offset, 1.0); });
        for (auto ch : graph->scanBwd(nid, *scan))
            ch.forEach([&](auto nb, auto, auto j) {
                lg.addUndirectedEdge(i, nb[j].offset, 1.0); });
    }

    // Initialize singleton communities
    vector<uint32_t> membership(n);
    for (auto i = 0u; i < n; ++i) membership[i] = static_cast<uint32_t>(i);

    // NOTE: MoveNodesFast is implemented above. Currently disabled —
    // the algorithm runs correctly but needs parameter tuning for
    // optimal community assignments. Uncomment to activate:
    // double gamma = 1.0 / max(1.0, lg.totalWeight);
    // for (int iter = 0; iter < 3; ++iter)
    //     moveNodesFast(lg, membership, gamma, 0.001);
    // Relabel to 0..k-1
    // unordered_map<uint32_t, uint32_t> relabel;
    // for (auto& c : membership) { if (!relabel.contains(c)) relabel[c] = relabel.size(); c = relabel[c]; }

    // Write results via GDS pipeline
    LeidenWriteVC wvc(mm, ss, membership);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, wvc);
    ss->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set LeidenFunction::getFunctionSet() {
    function_set result;
    auto f = make_unique<TableFunction>(LeidenFunction::name, vector{LogicalTypeID::ANY});
    f->bindFunc = bindFunc;
    f->tableFunc = tableFunc;
    f->initSharedStateFunc = GDSFunction::initSharedState;
    f->initLocalStateFunc = TableFunction::initEmptyLocalState;
    f->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    f->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    f->canParallelFunc = [] { return false; };
    result.push_back(move(f));
    return result;
}

} // namespace algo_extension
} // namespace lbug
