#pragma once

#include <memory>
#include <string>

#include "common/types/types.h"
#include <arrow/api.h>

namespace lbug {
namespace main {
class ClientContext;
} // namespace main
namespace graph {
class Graph;
} // namespace graph
namespace storage {
class MemoryManager;
} // namespace storage

namespace algo_extension {

// Symmetric (undirected) SIMPLE adjacency of the projected graph as Arrow CSR, ready to hand
// to NetworKit::GraphR. Shared by every icebug-backed GDS function. Both construction paths
// coalesce parallel edges and reciprocal pairs (symmetrize()'s A + A.T semantics): GDS
// operates on the simple undirected projection.
struct BridgeCSR {
    std::shared_ptr<arrow::UInt64Array> indptr;  // length numNodes + 1
    std::shared_ptr<arrow::UInt64Array> indices; // length numEdges (symmetric, per-row sorted
                                                 // on the zero-copy path)
};

// Build the undirected CSR for a single-node-table projected graph.
//
// Fast path (zero-copy): PROJECT_GRAPH pins each rel table's arrow CSR on the graph entry at
// projection time; we take that, symmetrize() it (A + A.T, per-row sorted), and reinterpret the
// int64 buffers as uint64 without copying. Falls back to scanning storage (fwd + bwd) into an
// InMemGraph whenever the pinned CSR is unavailable (filtered projection, multi-node-table
// graph, manual-transaction projection, stale dimensions).
BridgeCSR buildUndirectedCSR(main::ClientContext* context, const std::string& graphName,
    graph::Graph* graph, common::table_id_t tableID, common::offset_t numNodes,
    storage::MemoryManager* mm);

} // namespace algo_extension
} // namespace lbug
