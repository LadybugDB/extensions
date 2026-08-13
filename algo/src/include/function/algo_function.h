#pragma once

#include "function/function.h"

namespace lbug {
namespace algo_extension {

struct SCCFunction {
    static constexpr const char* name = "STRONGLY_CONNECTED_COMPONENTS";

    static function::function_set getFunctionSet();
};

struct SCCAliasFunction {
    using alias = SCCFunction;

    static constexpr const char* name = "SCC";
};

struct SCCKosarajuFunction {
    static constexpr const char* name = "STRONGLY_CONNECTED_COMPONENTS_KOSARAJU";

    static function::function_set getFunctionSet();
};

struct SCCKosarajuAliasFunction {
    using alias = SCCKosarajuFunction;

    static constexpr const char* name = "SCC_KO";
};

struct WeaklyConnectedComponentsFunction {
    static constexpr const char* name = "WEAKLY_CONNECTED_COMPONENTS";

    static function::function_set getFunctionSet();
};

struct WeaklyConnectedComponentsAliasFunction {
    using alias = WeaklyConnectedComponentsFunction;

    static constexpr const char* name = "WCC";
};

struct PageRankFunction {
    static constexpr const char* name = "PAGE_RANK";

    static function::function_set getFunctionSet();
};

struct PageRankAliasFunction {
    using alias = PageRankFunction;

    static constexpr const char* name = "PR";
};

// icebug (NetworKit)-backed PageRank. Coexists with PAGE_RANK for one release cycle.
struct GDSPageRankFunction {
    static constexpr const char* name = "GDS_PAGE_RANK";

    static function::function_set getFunctionSet();
};

// icebug (NetworKit)-backed Node2Vec structural embeddings.
struct GDSNode2VecFunction {
    static constexpr const char* name = "GDS_NODE2VEC";

    static function::function_set getFunctionSet();
};

// icebug (NetworKit)-backed Louvain community detection. Coexists with LOUVAIN for one release
// cycle.
struct GDSLouvainFunction {
    static constexpr const char* name = "GDS_LOUVAIN";

    static function::function_set getFunctionSet();
};

// icebug (NetworKit)-backed Leiden community detection (ParallelLeidenView) — higher partition
// quality and performance than Louvain; prefer for new work.
struct GDSLeidenFunction {
    static constexpr const char* name = "GDS_LEIDEN";

    static function::function_set getFunctionSet();
};

// icebug (NetworKit)-backed personalized PageRank (random walk with restart from caller-supplied
// source nodes) — scores measure standing relative to the sources, not globally.
struct GDSPprFunction {
    static constexpr const char* name = "GDS_PPR";

    static function::function_set getFunctionSet();
};

struct KCoreDecompositionFunction {
    static constexpr const char* name = "K_CORE_DECOMPOSITION";

    static function::function_set getFunctionSet();
};

struct KCoreDecompositionAliasFunction {
    using alias = KCoreDecompositionFunction;

    static constexpr const char* name = "KCORE";
};

struct LouvainFunction {
    static constexpr const char* name = "LOUVAIN";

    static function::function_set getFunctionSet();
};

struct SpanningForest {
    static constexpr const char* name = "SPANNING_FOREST";

    static function::function_set getFunctionSet();
};

struct SpanningForestAliasFunction {
    using alias = SpanningForest;

    static constexpr const char* name = "SF";
};

} // namespace algo_extension
} // namespace lbug
