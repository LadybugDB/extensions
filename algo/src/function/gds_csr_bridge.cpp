// The shared projected-graph -> Arrow CSR bridge for icebug-backed GDS functions.
//
// Zero-copy path: PROJECT_GRAPH materializes each rel table's scan as arrow CSR and pins the
// ArrowQueryResult on the graph entry (ladybug#800). We fetch it here, symmetrize() it into the
// undirected view (ladybug#799 — per-row sorted, reciprocal pairs coalesced), import the C-ABI
// ArrowArrays into arrow::Arrays, and reinterpret the int64 buffers as uint64 in place. No edge
// is copied at any step.
//
// Fallback path: scan storage fwd + bwd into an InMemGraph and build the arrays with a builder —
// the original bridge path, kept for projections the pinned CSR can't represent (filters,
// multi-node-table graphs, manual-transaction projections, stale dimensions).
#include "function/gds_csr_bridge.h"

#include <algorithm>

#include "common/in_mem_graph.h"
#include "graph/graph.h"
#include "graph/graph_entry_set.h"
#include "graph/parsed_graph_entry.h"
#include "main/client_context.h"
#include "main/query_result/arrow_query_result.h"
#include "storage/storage_manager.h"
#include "storage/table/table.h"
#include <arrow/c/bridge.h>

using namespace lbug::common;
using namespace lbug::graph;

namespace lbug {
namespace algo_extension {

// Import a C-ABI ArrowArray (int64) and reinterpret it as uint64 without copying: rowids are
// non-negative, and the buffer layout is identical. The imported array's release callback keeps
// the underlying buffers alive for as long as the returned array (and anything constructed over
// its buffers, like GraphR) is held.
static std::shared_ptr<arrow::UInt64Array> importAsU64(
    main::ArrowQueryResult::CSRArrowArray&& csrArray) {
    auto imported = arrow::ImportArray(&csrArray.array, &csrArray.schema);
    if (!imported.ok()) {
        return nullptr;
    }
    auto data = (*imported)->data()->Copy(); // shallow: buffers are shared, not copied
    data->type = arrow::uint64();
    return std::make_shared<arrow::UInt64Array>(std::move(data));
}

// The zero-copy path; returns {nullptr, nullptr} whenever any precondition fails so the caller
// falls back to the scan path.
static BridgeCSR fromMaterializedCsr(main::ClientContext* context, const std::string& graphName,
    Graph* graph, table_id_t tableID, offset_t numNodes) {
    if (graphName.empty()) {
        return {};
    }
    auto* entrySet = GraphEntrySet::Get(*context);
    if (!entrySet->hasGraph(graphName)) {
        return {};
    }
    auto* parsed = entrySet->getEntry(graphName);
    if (parsed->type != GraphEntryType::NATIVE) {
        return {};
    }
    auto& native = parsed->cast<ParsedNativeGraphEntry>();
    // MVP mirrors the GDS functions: single rel table, first entry.
    if (native.relCsrResults.empty() || native.relCsrResults[0] == nullptr) {
        return {};
    }
    auto* arrowResult = dynamic_cast<main::ArrowQueryResult*>(native.relCsrResults[0].get());
    if (arrowResult == nullptr || !arrowResult->hasCSRMetadata()) {
        return {};
    }
    // Staleness check: the CSR was pinned at projection time with the rel table's change epoch.
    // Any mutation since (even one leaving node cardinality unchanged) bumps the epoch — serve
    // live storage via the fallback instead of a stale snapshot.
    if (native.relCsrEpochs.size() != native.relCsrResults.size()) {
        return {};
    }
    const auto nbrTables = graph->getRelInfos(tableID);
    if (nbrTables.empty()) {
        return {};
    }
    const auto* relTable =
        storage::StorageManager::Get(*context)->getTable(nbrTables[0].relTableID);
    if (relTable == nullptr || relTable->getChangeEpoch() != native.relCsrEpochs[0]) {
        return {};
    }
    auto symmetric = arrowResult->getCSRArrowArrays().symmetrize();
    auto indptr = importAsU64(std::move(symmetric.indptr));
    auto indices = importAsU64(std::move(symmetric.indices));
    if (indptr == nullptr || indices == nullptr) {
        return {};
    }
    // Dimension check: the pinned CSR reflects projection-time cardinality. If the node table
    // grew since, offsets would be out of range — fall back to a fresh scan.
    if (static_cast<offset_t>(indptr->length()) != numNodes + 1) {
        return {};
    }
    return {std::move(indptr), std::move(indices)};
}

// The fallback: materialize the undirected adjacency by scanning storage. Per-row sort + unique
// coalesces parallel edges and reciprocal pairs so both paths build the same SIMPLE undirected
// graph — symmetrize()'s A + A.T semantics. (The pre-bridge scan kept multiplicity; that made
// degree-sensitive algorithms diverge between the two paths on multigraph data.)
static void scanCSR(table_id_t tableID, offset_t numNodes, Graph* graph, InMemGraph& inMem) {
    const auto nbrTables = graph->getRelInfos(tableID);
    const auto nbrInfo = nbrTables[0];
    const auto scanState = graph->prepareRelScan(*nbrInfo.relGroupEntry, nbrInfo.relTableID,
        nbrInfo.dstTableID, {}, false /*randomLookup*/);
    std::vector<offset_t> nbrs;
    for (offset_t nodeId = 0; nodeId < numNodes; ++nodeId) {
        nbrs.clear();
        const nodeID_t nid = {nodeId, tableID};
        for (auto chunk : graph->scanFwd(nid, *scanState)) {
            chunk.forEach(
                [&](auto neighbors, auto, auto i) { nbrs.push_back(neighbors[i].offset); });
        }
        for (auto chunk : graph->scanBwd(nid, *scanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                if (neighbors[i].offset != nodeId) {
                    nbrs.push_back(neighbors[i].offset);
                }
            });
        }
        std::sort(nbrs.begin(), nbrs.end());
        nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
        inMem.initNextNode();
        for (const auto nbr : nbrs) {
            inMem.insertNbr(nbr);
        }
    }
    inMem.initNextNode(); // trailing sentinel: csrOffsets[numNodes] == numEdges
}

static std::shared_ptr<arrow::UInt64Array> toU64(const std::function<offset_t(offset_t)>& at,
    offset_t count) {
    arrow::UInt64Builder builder;
    (void)builder.Reserve(count);
    for (offset_t i = 0; i < count; ++i) {
        (void)builder.Append(static_cast<uint64_t>(at(i)));
    }
    std::shared_ptr<arrow::Array> arr;
    (void)builder.Finish(&arr);
    return std::static_pointer_cast<arrow::UInt64Array>(arr);
}

BridgeCSR buildUndirectedCSR(main::ClientContext* context, const std::string& graphName,
    Graph* graph, table_id_t tableID, offset_t numNodes, storage::MemoryManager* mm) {
    auto materialized = fromMaterializedCsr(context, graphName, graph, tableID, numNodes);
    if (materialized.indptr != nullptr && materialized.indices != nullptr) {
        return materialized;
    }
    InMemGraph inMem(numNodes, mm);
    scanCSR(tableID, numNodes, graph, inMem);
    auto indptr = toU64([&](offset_t i) { return inMem.csrOffsets[i]; }, numNodes + 1);
    auto indices =
        toU64([&](offset_t i) { return inMem.csrEdges[i].neighbor; }, inMem.csrEdges.size());
    return {std::move(indptr), std::move(indices)};
}

} // namespace algo_extension
} // namespace lbug
