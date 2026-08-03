// LadybugDB ALGO Extension — Leiden community detection
//
// Full implementation: Local Moving (inherited from Louvain pattern) +
//                      Refinement (Leiden-unique) + Aggregation (shared)
//
// Reference: Traag, Waltman & van Eck (2019)
//   "From Louvain to Leiden: guaranteeing well-connected communities"
//   https://www.nature.com/articles/s41598-019-41695-z

#include "function/leiden.h"

#include "binder/binder.h"
#include "common/in_mem_gds_utils.h"
#include "common/in_mem_graph.h"
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
#include <atomic>
#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {
namespace { // anonymous — avoid link conflict with louvain.cpp's identical symbols

// ═══════════════════════════════════════════════════════════════════
// Section 1: Shared data structures (inherited from louvain.cpp)
// ═══════════════════════════════════════════════════════════════════

constexpr double THRESHOLD = 1e-6;
constexpr offset_t UNASSIGNED_COMM = numeric_limits<offset_t>::max();

static constexpr const char* LEIDEN_COLUMN_NAME = "community_id";
using LeidenBindData = GDSBindData;

struct CommInfo {
    atomic<offset_t> size;
    atomic<weight_t> degree;

    CommInfo() : size{0}, degree(0) {}
    CommInfo(const CommInfo& other) {
        size.store(other.size.load());
        degree.store(other.degree.load());
    }
    CommInfo& operator=(const CommInfo& other) {
        if (this != &other) {
            size.store(other.size.load());
            degree.store(other.degree.load());
        }
        return *this;
    }
};

struct PhaseState {
    InMemGraph graph;
    AtomicObjectArray<offset_t> acceptedComm;
    AtomicObjectArray<offset_t> currComm;
    AtomicObjectArray<offset_t> nextComm;
    ObjectArray<CommInfo> currCommInfos;
    ObjectArray<CommInfo> nextCommInfos;
    AtomicObjectArray<weight_t> nodeWeightedDegrees;
    AtomicObjectArray<weight_t> selfCommWeights;
    weight_t totalWeight = 0;
    double modularityConstant = 0.0;

    PhaseState(const offset_t numNodes, MemoryManager* mm, ExecutionContext* context)
        : graph{InMemGraph(numNodes, mm)} {
        reinit(numNodes, mm, context);
    }
    DELETE_BOTH_COPY(PhaseState);

    void reinit(offset_t numNodes, MemoryManager* mm, ExecutionContext* context);
    void startNewIter(MemoryManager* mm, ExecutionContext* context);

    void initNextNode(const offset_t nodeId) {
        graph.initNextNode();
        currCommInfos.getUnsafe(nodeId).size.store(1, memory_order_relaxed);
        currCommInfos.getUnsafe(nodeId).degree.store(0, memory_order_relaxed);
        acceptedComm.set(nodeId, nodeId, memory_order_relaxed);
        currComm.set(nodeId, nodeId, memory_order_relaxed);
    }

    void insertNbr(const offset_t from, const offset_t to, const weight_t weight = DEFAULT_WEIGHT) {
        graph.insertNbr(to, weight);
        nodeWeightedDegrees.fetchAdd(from, weight, memory_order_relaxed);
        currCommInfos.getUnsafe(from).degree.fetch_add(weight, memory_order_relaxed);
        totalWeight += weight;
    }

    void finalize() { graph.initNextNode(); }
};

// ═══════════════════════════════════════════════════════════════════
// Section 2: Parallel VC helpers (inherited from louvain.cpp)
// ═══════════════════════════════════════════════════════════════════

class ResetPhaseStateVC final : public InMemParallelCompute {
public:
    explicit ResetPhaseStateVC(PhaseState& state) : state{state} {}
    ~ResetPhaseStateVC() override = default;

    void parallelCompute(const offset_t startOffset, const offset_t endOffset,
        const optional<table_id_t>&) override {
        for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
            state.nodeWeightedDegrees.set(nodeId, 0, memory_order_relaxed);
            state.currCommInfos.set(nodeId, CommInfo());
            state.acceptedComm.set(nodeId, UNASSIGNED_COMM, memory_order_relaxed);
            state.currComm.set(nodeId, UNASSIGNED_COMM, memory_order_relaxed);
            state.nextComm.set(nodeId, UNASSIGNED_COMM, memory_order_relaxed);
        }
    }
    unique_ptr<InMemParallelCompute> copy() override {
        return make_unique<ResetPhaseStateVC>(state);
    }
private:
    PhaseState& state;
};

class StartNewIterVC final : public InMemParallelCompute {
public:
    explicit StartNewIterVC(PhaseState& state) : state{state} {}
    ~StartNewIterVC() override = default;

    void parallelCompute(const offset_t startOffset, const offset_t endOffset,
        const optional<table_id_t>&) override {
        for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
            state.selfCommWeights.set(nodeId, 0, memory_order_relaxed);
            state.nextCommInfos.set(nodeId, CommInfo());
        }
    }
    unique_ptr<InMemParallelCompute> copy() override {
        return make_unique<StartNewIterVC>(state);
    }
private:
    PhaseState& state;
};

void PhaseState::reinit(const offset_t numNodes, MemoryManager* mm, ExecutionContext* context) {
    totalWeight = 0;
    graph.reinit(numNodes);
    nodeWeightedDegrees.reallocate(numNodes, mm);
    currCommInfos.reallocate(numNodes, mm);
    acceptedComm.reallocate(numNodes, mm);
    currComm.reallocate(numNodes, mm);
    nextComm.reallocate(numNodes, mm);

    ResetPhaseStateVC resetPhaseStateVC(*this);
    InMemGDSUtils::runParallelCompute(resetPhaseStateVC, numNodes, context);
}

void PhaseState::startNewIter(MemoryManager* mm, ExecutionContext* context) {
    selfCommWeights.reallocate(graph.numNodes, mm);
    nextCommInfos.reallocate(graph.numNodes, mm);

    StartNewIterVC startNewIterVC(*this);
    InMemGDSUtils::runParallelCompute(startNewIterVC, graph.numNodes, context);

    modularityConstant = 1.0 / totalWeight;
}

// ═══════════════════════════════════════════════════════════════════
// Section 3: Louvain Phase 1 — Parallel Local Moving (inherited)
// ═══════════════════════════════════════════════════════════════════

class RunIterationVC final : public InMemParallelCompute {
public:
    explicit RunIterationVC(PhaseState& state) : state{state} {}
    ~RunIterationVC() override = default;

    void parallelCompute(const offset_t startOffset, const offset_t endOffset,
        const optional<table_id_t>&) override {
        vector<weight_t> intraCommWeights;
        unordered_map<offset_t, offset_t> commToWeightsIndex;
        for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
            const auto startCSROffset = state.graph.csrOffsets[nodeId];
            const auto endCSROffset = state.graph.csrOffsets[nodeId + 1];
            offset_t targetCommId = UNASSIGNED_COMM;
            if (startCSROffset != endCSROffset) {
                commToWeightsIndex.clear();
                intraCommWeights.clear();
                const weight_t selfLoopWeight = computeIntraCommWeights(nodeId, startCSROffset,
                    endCSROffset, intraCommWeights, commToWeightsIndex);
                targetCommId = findPotentialNewComm(nodeId, selfLoopWeight, intraCommWeights,
                    commToWeightsIndex);
                state.selfCommWeights.set(nodeId, intraCommWeights[0], memory_order_relaxed);
            }
            state.nextComm.set(nodeId, targetCommId, memory_order_relaxed);
            const auto currCommId = state.currComm.get(nodeId, memory_order_relaxed);
            if (targetCommId != currCommId && targetCommId != UNASSIGNED_COMM) {
                const auto nodeDegree = state.nodeWeightedDegrees.get(nodeId, memory_order_relaxed);
                state.nextCommInfos.getUnsafe(targetCommId).degree.fetch_add(nodeDegree);
                state.nextCommInfos.getUnsafe(targetCommId).size.fetch_add(1);
                state.nextCommInfos.getUnsafe(currCommId).degree.fetch_sub(nodeDegree);
                state.nextCommInfos.getUnsafe(currCommId).size.fetch_sub(1);
            }
        }
    }

    weight_t computeIntraCommWeights(const offset_t nodeId, const offset_t startCSROffset,
        const offset_t endCSROffset, vector<weight_t>& intraCommWeights,
        unordered_map<offset_t, offset_t>& commToWeightsIndex) const {
        weight_t selfLoopWeight = 0;
        const auto currComm = state.currComm.get(nodeId, memory_order_relaxed);
        commToWeightsIndex[currComm] = 0;
        intraCommWeights.push_back(0);
        offset_t nextIndex = 1;
        for (auto offset = startCSROffset; offset < endCSROffset; offset++) {
            auto nbrEntry = state.graph.csrEdges[offset];
            if (nbrEntry.neighbor == nodeId) {
                selfLoopWeight += nbrEntry.weight;
            }
            auto nbrCommId = state.currComm.get(nbrEntry.neighbor, memory_order_relaxed);
            if (!commToWeightsIndex.contains(nbrCommId)) {
                commToWeightsIndex[nbrCommId] = nextIndex;
                nextIndex++;
                intraCommWeights.push_back(nbrEntry.weight);
            } else {
                intraCommWeights[commToWeightsIndex[nbrCommId]] += nbrEntry.weight;
            }
        }
        return selfLoopWeight;
    }

    offset_t findPotentialNewComm(const offset_t nodeId, const weight_t selfLoopWeight,
        const vector<weight_t>& intraCommWeights,
        unordered_map<offset_t, offset_t> commToWeightsIndex) const {
        const auto currComm = state.currComm.get(nodeId, memory_order_relaxed);
        const auto degree =
            static_cast<double>(state.nodeWeightedDegrees.get(nodeId, memory_order_relaxed));
        auto newComm = currComm;
        double newCommModGain = 0.0;
        const auto prevIntraCommWeights = static_cast<double>(intraCommWeights[0] - selfLoopWeight);
        const auto prevWeightedDegrees =
            static_cast<double>(
                state.currCommInfos.getUnsafe(currComm).degree.load(memory_order_relaxed)) -
            degree;
        for (auto [nbrCommId, weightIndex] : commToWeightsIndex) {
            if (currComm != nbrCommId) {
                const auto newIntraCommWeights = static_cast<double>(intraCommWeights[weightIndex]);
                const auto newWeightedDegrees = static_cast<double>(
                    state.currCommInfos.getUnsafe(nbrCommId).degree.load(memory_order_relaxed));
                const auto changeIntraWeights = 2 * (newIntraCommWeights - prevIntraCommWeights);
                const auto changeSumWeightedDegrees = 2 * degree * state.modularityConstant *
                                                      (newWeightedDegrees - prevWeightedDegrees);
                const auto modGain = changeIntraWeights - changeSumWeightedDegrees;
                if (modGain > newCommModGain || ((newCommModGain - modGain) < THRESHOLD &&
                                                    modGain != 0 && (nbrCommId < newComm))) {
                    newCommModGain = modGain;
                    newComm = nbrCommId;
                }
            }
        }
        if (state.currCommInfos.getUnsafe(newComm).size.load(memory_order_relaxed) == 1 &&
            state.currCommInfos.getUnsafe(currComm).size.load(memory_order_relaxed) == 1 &&
            newComm > currComm) {
            newComm = currComm;
        }
        return newComm;
    }

    unique_ptr<InMemParallelCompute> copy() override {
        return make_unique<RunIterationVC>(state);
    }
private:
    PhaseState& state;
};

// ═══════════════════════════════════════════════════════════════════
// Section 4: Graph I/O — multi-table support
// ═══════════════════════════════════════════════════════════════════
//
// Builds an in-memory CSR graph from the GDS projection graph.
// Supports heterogeneous graphs with multiple node tables by using
// a flat index mapping: flatIdx = nodeOffsetBase[tableID] + offset.
// All PhaseState arrays are sized for totalNodes across all tables.

static void initInMemoryGraph(const vector<table_id_t>& nodeTableIDs,
    const table_id_map_t<offset_t>& nodeOffsetBase, offset_t totalNodes,
    Graph* graph, transaction::Transaction* transaction, PhaseState& state) {
    // Find the first relationship info from any node table
    table_id_t srcTableID = INVALID_TABLE_ID;
    table_id_t dstTableID = INVALID_TABLE_ID;
    const catalog::TableCatalogEntry* relGroupEntry = nullptr;
    oid_t relTableID = 0;
    for (auto tid : nodeTableIDs) {
        auto nbrTables = graph->getRelInfos(tid);
        if (!nbrTables.empty()) {
            srcTableID = nbrTables[0].srcTableID;
            dstTableID = nbrTables[0].dstTableID;
            relGroupEntry = nbrTables[0].relGroupEntry;
            relTableID = nbrTables[0].relTableID;
            break;
        }
    }

    if (relGroupEntry == nullptr) {
        // No edges in this graph — just initialize all nodes
        for (auto tableID : nodeTableIDs) {
            auto numNodes = graph->getMaxOffset(transaction, tableID);
            offset_t baseOffset = nodeOffsetBase.at(tableID);
            for (auto nodeId = 0u; nodeId < numNodes; ++nodeId) {
                state.initNextNode(baseOffset + nodeId);
            }
        }
        state.finalize();
        return;
    }

    // Prepare FWD scan state (neighbors are in dstTableID)
    auto fwdState = graph->prepareRelScan(*relGroupEntry, relTableID, dstTableID, {}, false);
    // Prepare BWD scan state (neighbors are in srcTableID)
    auto bwdState = graph->prepareRelScan(*relGroupEntry, relTableID, srcTableID, {}, false);

    // Process all node tables
    for (auto tableID : nodeTableIDs) {
        auto numNodes = graph->getMaxOffset(transaction, tableID);
        offset_t baseOffset = nodeOffsetBase.at(tableID);

        for (auto nodeId = 0u; nodeId < numNodes; ++nodeId) {
            offset_t flatNodeIdx = baseOffset + nodeId;
            state.initNextNode(flatNodeIdx);
            const nodeID_t nextNodeId = {nodeId, tableID};

            // FWD scan: outgoing neighbors (convert to flat indices)
            for (auto chunk : graph->scanFwd(nextNodeId, *fwdState)) {
                chunk.forEach([&](auto neighbors, auto, auto i) {
                    offset_t flatNbr = nodeOffsetBase.at(neighbors[i].tableID) + neighbors[i].offset;
                    state.insertNbr(flatNodeIdx, flatNbr);
                });
            }
            // BWD scan: incoming neighbors (convert to flat indices)
            for (auto chunk : graph->scanBwd(nextNodeId, *bwdState)) {
                chunk.forEach([&](auto neighbors, auto, auto i) {
                    offset_t flatNbr = nodeOffsetBase.at(neighbors[i].tableID) + neighbors[i].offset;
                    if (flatNbr != flatNodeIdx) {
                        state.insertNbr(flatNodeIdx, flatNbr);
                    }
                });
            }
        }
    }
    state.finalize();
}

static offset_t renumberCommunities(PhaseState& state) {
    unordered_map<offset_t, offset_t> map;
    offset_t nextCommId = 0;
    for (auto nodeId = 0LU; nodeId < state.graph.numNodes; ++nodeId) {
        auto commId = state.acceptedComm.get(nodeId, memory_order_relaxed);
        if (commId == UNASSIGNED_COMM) continue;
        if (!map.contains(commId)) {
            map.insert(make_pair(commId, nextCommId));
            nextCommId++;
        }
        state.acceptedComm.set(nodeId, map.at(commId), memory_order_relaxed);
    }
    return nextCommId;
}

static void aggregateCommunities(const offset_t newCommCount, PhaseState& state,
    MemoryManager* mm, ExecutionContext* context) {
    vector_t<unordered_map<offset_t, weight_t>> commWeights(mm);
    commWeights.resize(newCommCount);
    for (auto nodeId = 0u; nodeId < state.graph.numNodes; nodeId++) {
        const auto beginCSROffset = state.graph.csrOffsets[nodeId];
        const auto endCSROffset = state.graph.csrOffsets[nodeId + 1];
        auto commId = state.acceptedComm.get(nodeId, memory_order_relaxed);
        for (auto offset = beginCSROffset; offset < endCSROffset; ++offset) {
            const auto nbr = state.graph.csrEdges[offset];
            auto nbrCommId = state.acceptedComm.get(nbr.neighbor, memory_order_relaxed);
            if (commId >= nbrCommId) {
                commWeights[commId][nbrCommId] += nbr.weight;
                if (commId != nbrCommId) {
                    commWeights[nbrCommId][commId] += nbr.weight;
                }
            }
        }
    }
    state.reinit(newCommCount, mm, context);
    for (auto nodeId = 0u; nodeId < newCommCount; nodeId++) {
        state.initNextNode(nodeId);
        for (auto [nbrId, weight] : commWeights[nodeId]) {
            state.insertNbr(nodeId, nbrId, weight);
        }
    }
    state.finalize();
}

// ═══════════════════════════════════════════════════════════════════
// Section 5: Leiden Phase 2 — Refinement (the ONLY new algorithm)
// ═══════════════════════════════════════════════════════════════════
//
// Uses its own refined[] array (never touches state.currComm) to avoid
// polluting Phase 1's Local Moving state for subsequent phases.
//
// Refinement guarantees that every community is internally connected,
// which is the key differentiator from Louvain.
//
// Algorithm (from Traag et al. 2019, Algorithm 2):
//   1. Each node starts in its own refined community.
//   2. Constrained Local Moving: nodes can only merge within the same
//      PARENT community (from Phase 1).
//   3. Connectivity guarantee: reject moves that would disconnect the
//      source refined community (verified via BFS).
//   4. Singleton merge: lone nodes in a refined community get merged
//      into a random neighbor's refined community (avoids oversplitting).

// Edge-to-community weight map inside a single parent community.
// Key = refined community ID, Value = sum of edge weights from `node` to that community.
using CommunityWeightMap = unordered_map<offset_t, double>;

// BFS: check whether community `comm` in `refined` stays connected after removing `node`.
static bool staysConnectedAfterRemoval(const InMemGraph& g,
    const vector<offset_t>& refined, offset_t node, offset_t comm) {
    vector<offset_t> members;
    for (offset_t i = 0; i < g.numNodes; i++)
        if (i != node && refined[i] == comm) members.push_back(i);
    if (members.empty()) return true;

    unordered_set<offset_t> visited;
    deque<offset_t> q;
    q.push_back(members[0]);
    visited.insert(members[0]);

    while (!q.empty()) {
        offset_t cur = q.front(); q.pop_front();
        for (auto csrIdx = g.csrOffsets[cur]; csrIdx < g.csrOffsets[cur + 1]; csrIdx++) {
            offset_t nbr = g.csrEdges[csrIdx].neighbor;
            if (refined[nbr] == comm && !visited.contains(nbr)) {
                visited.insert(nbr);
                q.push_back(nbr);
            }
        }
    }
    return visited.size() == members.size();
}

// Per-node metadata for Refinement: {refinedComm, weightToSelf, weightToOthers}
struct RefineNodeMeta {
    offset_t refinedComm;
    double   degree;          // = state.nodeWeightedDegrees[node]
    CommunityWeightMap nbrWeights; // total edge weight to each refined community (within parent)
};

static void refinePartition(PhaseState& state,
    vector<offset_t>& refined,           // IN/OUT
    const vector<offset_t>& parentComm) // IN: Phase 1 membership per node
{
    offset_t n = state.graph.numNodes;
    if (n < 2) return;

    // 1. Each node starts in its own refined community.
    refined.resize(n);
    for (offset_t i = 0; i < n; i++) refined[i] = i;

    // 2. Build per-parent-community node lists.
    unordered_map<offset_t, vector<offset_t>> commNodes;
    for (offset_t i = 0; i < n; i++) commNodes[parentComm[i]].push_back(i);

    // 3. Per-node degree cache.
    vector<double> nodeDegree(n);
    for (offset_t i = 0; i < n; i++)
        nodeDegree[i] = static_cast<double>(
            state.nodeWeightedDegrees.get(i, memory_order_relaxed));

    // 4. Refinement degree tracking: commDeg[c] = sum of nodeDegree for nodes in refined community c.
    unordered_map<offset_t, double> commDeg;

    // Initialize each singleton refined community's degree.
    for (offset_t i = 0; i < n; i++) commDeg[i] = nodeDegree[i];

    // Helper: apply a move of node `u` from `fromC` to `toC`.
    auto applyMove = [&](offset_t u, offset_t fromC, offset_t toC) {
        refined[u] = toC;
        commDeg[fromC] -= nodeDegree[u];
        commDeg[toC]   += nodeDegree[u];
    };

    static mt19937 gen(random_device{}());

    // 5. Process each parent community independently.
    for (auto& [pc, nodes] : commNodes) {
        if (nodes.size() <= 1) continue;

        shuffle(nodes.begin(), nodes.end(), gen);
        int maxIters = 10;
        bool changed = true;

        while (changed && maxIters-- > 0) {
            changed = false;

            for (offset_t u : nodes) {
                offset_t curC = refined[u];
                CommunityWeightMap nbrWeights;

                // Collect edge weights to refined communities (within same parent only).
                for (auto csrIdx = state.graph.csrOffsets[u];
                     csrIdx < state.graph.csrOffsets[u + 1]; csrIdx++) {
                    const auto& nbr = state.graph.csrEdges[csrIdx];
                    if (parentComm[nbr.neighbor] != pc) continue;
                    offset_t nbrC = refined[nbr.neighbor];
                    nbrWeights[nbrC] += static_cast<double>(nbr.weight);
                }

                // Find best target refined community.
                offset_t bestC = curC;
                double   bestDelta = 0.0;

                for (auto& [nbrC, wTo] : nbrWeights) {
                    if (nbrC == curC) continue;

                    // Connectivity check: will `curC` stay connected after u leaves?
                    if (!staysConnectedAfterRemoval(state.graph, refined, u, curC))
                        continue;

                    // Modularity delta (simplified):
                    //   ΔQ = (w_to - w_from) / (2m)
                    //        - degree(u) * (deg_to - deg_from + degree(u)) / (2m)²
                    double wFrom = nbrWeights[curC];  // 0 if curC not in nbrWeights
                    double degFrom = commDeg[curC];
                    double degTo   = commDeg[nbrC];
                    double mConst  = state.modularityConstant;

                    double delta = (wTo - wFrom) * mConst
                        - nodeDegree[u] * (degTo - degFrom + nodeDegree[u]) * mConst * mConst;

                    if (delta > bestDelta) { bestDelta = delta; bestC = nbrC; }
                }

                if (bestC != curC && bestDelta > 0) {
                    applyMove(u, curC, bestC);
                    changed = true;
                }
            }
        }

        // 6. Merge singletons: a node alone in its refined community gets merged
        //    into a random neighbor's refined community (within the same parent).
        for (offset_t u : nodes) {
            offset_t myC = refined[u];
            // Count members of `myC` within this parent community.
            int count = 0;
            for (offset_t v : nodes) if (refined[v] == myC) count++;
            if (count > 1) continue; // not a singleton

            // Find any neighbor's refined community within the same parent.
            offset_t mergeTarget = UNASSIGNED_COMM;
            for (auto csrIdx = state.graph.csrOffsets[u];
                 csrIdx < state.graph.csrOffsets[u + 1]; csrIdx++) {
                offset_t nbr = state.graph.csrEdges[csrIdx].neighbor;
                if (parentComm[nbr] != pc) continue;
                if (refined[nbr] != myC) { mergeTarget = refined[nbr]; break; }
            }
            if (mergeTarget == UNASSIGNED_COMM) continue;

            applyMove(u, myC, mergeTarget);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Section 6: GDSOutput writers (inherited from louvain.cpp)
// ═══════════════════════════════════════════════════════════════════

struct FinalResults {
    vector<offset_t> communities;
    explicit FinalResults(const offset_t numNodes) { communities.resize(numNodes); }
};

class SaveCommAssignmentsVC final : public InMemParallelCompute {
public:
    SaveCommAssignmentsVC(const offset_t phaseId, FinalResults& finalResults, PhaseState& state)
        : phaseId{phaseId}, finalResults{finalResults}, state{state} {}
    ~SaveCommAssignmentsVC() override = default;

    void parallelCompute(const offset_t startOffset, const offset_t endOffset,
        const optional<table_id_t>&) override {
        if (phaseId == 0) {
            for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
                finalResults.communities[nodeId] =
                    state.acceptedComm.get(nodeId, memory_order_relaxed);
            }
        } else {
            for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
                const auto prevCommunity = finalResults.communities[nodeId];
                if (prevCommunity == UNASSIGNED_COMM) continue;
                const auto newCommunity =
                    state.acceptedComm.get(prevCommunity, memory_order_relaxed);
                finalResults.communities[nodeId] = newCommunity;
            }
        }
    }
    unique_ptr<InMemParallelCompute> copy() override {
        return make_unique<SaveCommAssignmentsVC>(phaseId, finalResults, state);
    }
private:
    offset_t phaseId;
    FinalResults& finalResults;
    PhaseState& state;
};

class ComputeModularityVC final : public InMemParallelCompute {
public:
    ComputeModularityVC(PhaseState& state, atomic<weight_t>& sumIntraWeights,
        atomic<weight_t>& sumWeightedDegrees)
        : state{state}, sumIntraWeights{sumIntraWeights}, sumWeightedDegrees{sumWeightedDegrees} {}
    ~ComputeModularityVC() override = default;

    void parallelCompute(const offset_t startOffset, const offset_t endOffset,
        const optional<table_id_t>&) override {
        weight_t sumIntraLocal = 0;
        weight_t sumTotalLocal = 0;
        for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
            sumIntraLocal += state.selfCommWeights.get(nodeId, memory_order_relaxed);
            const auto degree =
                state.currCommInfos.getUnsafe(nodeId).degree.load(memory_order_relaxed);
            sumTotalLocal += degree * degree;
        }
        sumIntraWeights.fetch_add(sumIntraLocal);
        sumWeightedDegrees.fetch_add(sumTotalLocal);
    }
    unique_ptr<InMemParallelCompute> copy() override {
        return make_unique<ComputeModularityVC>(state, sumIntraWeights, sumWeightedDegrees);
    }
private:
    PhaseState& state;
    atomic<weight_t>& sumIntraWeights;
    atomic<weight_t>& sumWeightedDegrees;
};

class UpdateCommInfosVC final : public InMemParallelCompute {
public:
    explicit UpdateCommInfosVC(PhaseState& state) : state{state} {}
    ~UpdateCommInfosVC() override = default;

    void parallelCompute(const offset_t startOffset, const offset_t endOffset,
        const optional<table_id_t>&) override {
        for (auto nodeId = startOffset; nodeId < endOffset; ++nodeId) {
            const offset_t size =
                state.nextCommInfos.getUnsafe(nodeId).size.load(memory_order_relaxed);
            const weight_t degree =
                state.nextCommInfos.getUnsafe(nodeId).degree.load(memory_order_relaxed);
            state.currCommInfos.getUnsafe(nodeId).size.fetch_add(size, memory_order_relaxed);
            state.currCommInfos.getUnsafe(nodeId).degree.fetch_add(degree, memory_order_relaxed);
        }
    }
    unique_ptr<InMemParallelCompute> copy() override {
        return make_unique<UpdateCommInfosVC>(state);
    }
private:
    PhaseState& state;
};

class LeidenWriteVC final : public GDSResultVertexCompute {
public:
    LeidenWriteVC(MemoryManager* mm, GDSFuncSharedState* ss,
        const vector<offset_t>& comm,
        const table_id_map_t<offset_t>& nodeOffsetBase)
        : GDSResultVertexCompute{mm, ss}, community{comm},
          nodeOffsetBase{nodeOffsetBase} {
        nodeIDVec = createVector(LogicalType::INTERNAL_ID());
        commIDVec = createVector(LogicalType::INT64());
    }

    void beginOnTableInternal(table_id_t) override {}
    void vertexCompute(offset_t start, offset_t end, const table_id_t tid) override {
        offset_t baseOff = nodeOffsetBase.at(tid);
        for (auto i = start; i < end; ++i) {
            nodeIDVec->setValue<nodeID_t>(0, {i, tid});
            commIDVec->setValue<int64_t>(0,
                static_cast<int64_t>(community[baseOff + i]));
            localFT->append(vectors);
        }
    }
    unique_ptr<VertexCompute> copy() override {
        return make_unique<LeidenWriteVC>(mm, sharedState, community, nodeOffsetBase);
    }
private:
    const vector<offset_t>& community;
    const table_id_map_t<offset_t>& nodeOffsetBase;
    unique_ptr<ValueVector> nodeIDVec, commIDVec;
};

// ═══════════════════════════════════════════════════════════════════
// Section 7: LEIDEN tableFunc — 3-Phase algorithm
// ═══════════════════════════════════════════════════════════════════

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    const auto clientContext = input.context->clientContext;
    const auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto mm = MemoryManager::Get(*clientContext);
    const auto graph = sharedState->graph.get();

    // Build flat offset map across all node tables
    const auto nodeTableIDs = graph->getNodeTableIDs();
    table_id_map_t<offset_t> nodeOffsetBase;
    offset_t totalNodes = 0;
    for (auto tid : nodeTableIDs) {
        nodeOffsetBase[tid] = totalNodes;
        totalNodes += graph->getMaxOffset(transaction, tid);
    }

    // Default params: 20 iterations, 20 phases (same as Louvain)
    constexpr uint64_t MAX_ITERS = 20;
    constexpr uint64_t MAX_PHASES = 20;

    auto progressBar = ProgressBar::Get(*clientContext);
    const auto steps = MAX_PHASES * MAX_ITERS;

    FinalResults finalResults(totalNodes);
    PhaseState state(totalNodes, mm, input.context);

    // Build initial in-memory graph from GDS graph (all node tables)
    initInMemoryGraph(nodeTableIDs, nodeOffsetBase, totalNodes, graph, transaction, state);

    // Leiden 3-phase loop
    for (auto phase = 0u; phase < MAX_PHASES; ++phase) {
        double oldMod = -1;

        // ═══ Phase 1: Local Moving (same as Louvain) ═══
        for (auto iter = 0u; iter < MAX_ITERS; ++iter) {
            double progress = static_cast<double>((phase + 1) * (iter + 1)) / steps;

            state.startNewIter(mm, input.context);

            RunIterationVC runIteration(state);
            InMemGDSUtils::runParallelCompute(runIteration, state.graph.numNodes, input.context);

            progressBar->updateProgress(input.context->queryID, progress * 0.33);

            atomic<weight_t> sumIntraWeights{0};
            atomic<weight_t> sumWeightedDegrees{0};
            ComputeModularityVC newModularityVC(state, sumIntraWeights, sumWeightedDegrees);
            InMemGDSUtils::runParallelCompute(newModularityVC, state.graph.numNodes, input.context);
            const double currMod =
                sumIntraWeights.load() * state.modularityConstant -
                (sumWeightedDegrees.load() * state.modularityConstant * state.modularityConstant);

            if (currMod - oldMod < THRESHOLD) break;
            oldMod = currMod;

            UpdateCommInfosVC updateCommInfosVC(state);
            InMemGDSUtils::runParallelCompute(updateCommInfosVC, state.graph.numNodes, input.context);

            swap(state.acceptedComm, state.currComm);
            swap(state.currComm, state.nextComm);
        }

        // ═══ Phase 2: Refinement (Leiden-unique) ═══
        // Extracts Phase 1 parent membership, runs constrained Local Moving
        // with connectivity guarantee, then writes result back to acceptedComm.
        {
            vector<offset_t> parentMembership(state.graph.numNodes);
            for (offset_t i = 0; i < state.graph.numNodes; i++)
                parentMembership[i] = state.acceptedComm.get(i, memory_order_relaxed);

            vector<offset_t> refined(state.graph.numNodes);
            refinePartition(state, refined, parentMembership);

            // Copy refined → acceptedComm (which aggregateCommunities reads)
            for (offset_t i = 0; i < state.graph.numNodes; i++)
                state.acceptedComm.set(i, refined[i], memory_order_relaxed);
        }

        progressBar->updateProgress(input.context->queryID, 0.66);

        // ═══ Phase 3: Aggregation (same as Louvain) ═══
        const auto oldCommCount = state.graph.numNodes;
        const auto newCommCount = renumberCommunities(state);

        SaveCommAssignmentsVC setFinalComms(phase, finalResults, state);
        InMemGDSUtils::runParallelCompute(setFinalComms, totalNodes, input.context);

        if (oldCommCount == newCommCount) break;

        aggregateCommunities(newCommCount, state, mm, input.context);
        progressBar->updateProgress(input.context->queryID, 1.0);
    }

    // Write results via GDS pipeline
    const auto parallelCompute = make_unique<LeidenWriteVC>(mm, sharedState,
        finalResults.communities, nodeOffsetBase);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *parallelCompute);

    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
// Section 8: bindFunc + getFunctionSet (unchanged registration)
// ═══════════════════════════════════════════════════════════════════

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
