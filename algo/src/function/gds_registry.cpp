// The GDS catalog: one row per icebug-backed table function. See gds_registry.h for the
// row checklist. Shapes and policies live in gds_generic_algo.h; knob specs in
// gds_param_specs.h (+ function::DampingFactor/Tolerance, MaxIterations).
//
// Coverage map (NetworKit module -> rows here):
//   centrality  PageRank, Betweenness, Degree, Closeness, Harmonic, Katz, Eigenvector, Core,
//             ParallelCore (SIGMOD'25 exact adaptive peeling, needs icebug >= 13.5)
//   community   PLM (Louvain), ParallelLeidenView (Leiden), PLP, CutClustering
//   components  ConnectedComponents
//   distance    BFS (single-source distances), PageRank::forSources (personalized)
//   embedding   Node2Vec (makeEmbeddingFunc also covers FastRP once the vendored prebuilt
//               ships networkit/embedding/FastRP.hpp — the builder is double-instantiable)
// Follow-up shapes (global scalars, edge scores, link prediction, SCD, generators, ...) need
// new builders; rows for them do not fit make*Func and are rejected at design time, not with
// a half-working CALL.

#include "function/gds_registry.h"

#include "catalog/catalog.h"
#include "function/config/max_iterations_config.h"
#include "function/config/page_rank_config.h"
#include "function/gds_generic_algo.h"
#include "function/gds_param_specs.h"
#include "main/database.h"
#include <networkit/centrality/Betweenness.hpp>
#include <networkit/centrality/Closeness.hpp>
#include <networkit/centrality/CoreDecomposition.hpp>
#include <networkit/centrality/DegreeCentrality.hpp>
#include <networkit/centrality/EigenvectorCentrality.hpp>
#include <networkit/centrality/HarmonicCloseness.hpp>
#include <networkit/centrality/KatzCentrality.hpp>
#include <networkit/centrality/PageRank.hpp>
#include <networkit/centrality/ParallelCoreDecomposition.hpp>
#include <networkit/community/CutClustering.hpp>
#include <networkit/community/PLM.hpp>
#include <networkit/community/PLP.hpp>
#include <networkit/community/ParallelLeidenView.hpp>
#include <networkit/components/ConnectedComponents.hpp>
#include <networkit/distance/BFS.hpp>
#include <networkit/embedding/Node2Vec.hpp>
#include <networkit/graph/GraphR.hpp>

namespace lbug {
namespace algo_extension {

namespace {

// --- Migrated rows (semantics identical to the retired per-algorithm gds_*.cpp files). ---

function::function_set makeGDSPageRank() {
    // MaxIterations is accepted + validated for CALL compatibility with PAGE_RANK; the
    // NetworKit iteration does not take a cap, so the value is unused (as before).
    return makeNodeScoreFunc<function::DampingFactor, function::Tolerance, MaxIterations>(
        "GDS_PAGE_RANK", "rank", GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::PageRank pr(g, params.template get<function::DampingFactor>(),
                params.template get<function::Tolerance>());
            pr.run();
            return pr.scores();
        });
}

function::function_set makeGDSPpr() {
    return makeNodeScoreFunc<function::DampingFactor, function::Tolerance>("GDS_PPR", "rank",
        GDSExtraKind::SOURCE_LIST, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto& extra) {
            const std::vector<NetworKit::node> sources{extra.sources.begin(), extra.sources.end()};
            auto pr = NetworKit::PageRank::forSources(g, sources,
                params.template get<function::DampingFactor>(),
                params.template get<function::Tolerance>());
            pr.run();
            return pr.scores();
        });
}

function::function_set makeGDSLouvain() {
    return makePartitionFunc<>("GDS_LOUVAIN", "community_id", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t numNodes, const auto&, const auto&) {
            NetworKit::PLM plm(g);
            plm.run();
            return partitionToIds(plm.getPartition(), numNodes);
        });
}

function::function_set makeGDSLeiden() {
    return makePartitionFunc<LeidenGamma, LeidenIterations>("GDS_LEIDEN", "community_id",
        GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t numNodes, const auto& params, const auto&) {
            NetworKit::ParallelLeidenView leiden(g,
                static_cast<int>(params.template get<LeidenIterations>()), /*randomize=*/true,
                params.template get<LeidenGamma>());
            leiden.run();
            return partitionToIds(leiden.getPartition(), numNodes);
        });
}

function::function_set makeGDSNode2Vec() {
    return makeEmbeddingFunc<float>("GDS_NODE2VEC", GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto&, const auto&) {
            NetworKit::Node2Vec n2v(g, /*P=*/1.0, /*Q=*/1.0, /*L=*/10, /*N=*/5, /*D=*/64);
            n2v.run();
            return n2v.getFeatures();
        });
}

// --- New rows: centrality (all share the scores() shape; only the signature differs). ---

function::function_set makeGDSBetweenness() {
    return makeNodeScoreFunc<Normalized>("GDS_BETWEENNESS", "score", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::Betweenness algo(g, params.template get<Normalized>());
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSDegree() {
    return makeNodeScoreFunc<Normalized>("GDS_DEGREE", "score", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::DegreeCentrality algo(g, params.template get<Normalized>());
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSCloseness() {
    return makeNodeScoreFunc<NormalizedDefaultTrue>("GDS_CLOSENESS", "score", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            // Three-arg form: the two-arg call is ambiguous between the
            // (normalized, variant) and the legacy (normalized, checkConnectedness) ctors.
            NetworKit::Closeness algo(g, params.template get<NormalizedDefaultTrue>(),
                /*checkConnectedness=*/true);
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSHarmonic() {
    return makeNodeScoreFunc<NormalizedDefaultTrue>("GDS_HARMONIC", "score", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::HarmonicCloseness algo(g, params.template get<NormalizedDefaultTrue>());
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSKatz() {
    return makeNodeScoreFunc<KatzAlpha, KatzBeta, function::Tolerance>("GDS_KATZ", "score",
        GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::KatzCentrality algo(g, params.template get<KatzAlpha>(),
                params.template get<KatzBeta>(), params.template get<function::Tolerance>());
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSEigenvector() {
    return makeNodeScoreFunc<function::Tolerance>("GDS_EIGENVECTOR", "score", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::EigenvectorCentrality algo(g, params.template get<function::Tolerance>());
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSCore() {
    return makeNodeScoreFunc<Normalized>("GDS_CORE_DECOMPOSITION", "core_number",
        GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::CoreDecomposition algo(g, params.template get<Normalized>());
            algo.run();
            return algo.scores();
        });
}

function::function_set makeGDSParallelCore() {
    return makeNodeScoreFunc<Normalized, KCoreUseSampling>("GDS_PARALLEL_CORE_DECOMPOSITION",
        "core_number", GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
            NetworKit::ParallelCoreDecomposition algo(g, params.template get<Normalized>(),
                params.template get<KCoreUseSampling>());
            algo.run();
            return algo.scores();
        });
}

// --- New rows: community / components (Partition shape). ---

function::function_set makeGDSConnectedComponents() {
    return makePartitionFunc<>("GDS_CONNECTED_COMPONENTS", "component_id", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t numNodes, const auto&, const auto&) {
            NetworKit::ConnectedComponents algo(g);
            algo.run();
            return partitionToIds(algo.getPartition(), numNodes);
        });
}

function::function_set makeGDSPlp() {
    return makePartitionFunc<PLPTheta, PLPMaxIterations>("GDS_PLP", "community_id",
        GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t numNodes, const auto& params, const auto&) {
            const auto theta = params.template get<PLPTheta>() < 0 ?
                                   NetworKit::none :
                                   static_cast<NetworKit::count>(params.template get<PLPTheta>());
            const auto maxIter =
                params.template get<PLPMaxIterations>() < 0 ?
                    NetworKit::none :
                    static_cast<NetworKit::count>(params.template get<PLPMaxIterations>());
            NetworKit::PLP algo(g, theta, maxIter);
            algo.run();
            return partitionToIds(algo.getPartition(), numNodes);
        });
}

function::function_set makeGDSCutClustering() {
    // alpha is REQUIRED: CutClustering takes it as a constructor argument with no default.
    return makePartitionFunc<CutAlpha>("GDS_CUT_CLUSTERING", "community_id", GDSExtraKind::NONE,
        GDSGraphMode::UNDIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t numNodes, const auto& params, const auto&) {
            NetworKit::CutClustering algo(g, params.template get<CutAlpha>());
            algo.run();
            return partitionToIds(algo.getPartition(), numNodes);
        });
}

// --- New rows: single-source distances (directed) and embeddings. ---

function::function_set makeGDSBfsDistance() {
    return makeNodeScoreFunc<>("GDS_BFS_DISTANCE", "distance", GDSExtraKind::SINGLE_SOURCE,
        GDSGraphMode::DIRECTED,
        [](const NetworKit::GraphR& g, common::offset_t numNodes, const auto&, const auto& extra) {
            NetworKit::BFS algo(g, extra.sources[0]);
            algo.run();
            const auto& dist = algo.getDistances();
            std::vector<double> out(numNodes, -1.0);
            for (common::offset_t i = 0; i < numNodes && i < (common::offset_t)dist.size(); ++i) {
                out[i] = normalizeDistance(dist[i]);
            }
            return out;
        });
}

} // namespace

const std::vector<GDSCatalogEntry>& getGDSCatalog() {
    static const std::vector<GDSCatalogEntry> catalog = {
        {"GDS_PAGE_RANK", makeGDSPageRank},
        {"GDS_PPR", makeGDSPpr},
        {"GDS_LOUVAIN", makeGDSLouvain},
        {"GDS_LEIDEN", makeGDSLeiden},
        {"GDS_NODE2VEC", makeGDSNode2Vec},
        {"GDS_BETWEENNESS", makeGDSBetweenness},
        {"GDS_DEGREE", makeGDSDegree},
        {"GDS_CLOSENESS", makeGDSCloseness},
        {"GDS_HARMONIC", makeGDSHarmonic},
        {"GDS_KATZ", makeGDSKatz},
        {"GDS_EIGENVECTOR", makeGDSEigenvector},
        {"GDS_CORE_DECOMPOSITION", makeGDSCore},
        {"GDS_PARALLEL_CORE_DECOMPOSITION", makeGDSParallelCore},
        {"GDS_CONNECTED_COMPONENTS", makeGDSConnectedComponents},
        {"GDS_PLP", makeGDSPlp},
        {"GDS_CUT_CLUSTERING", makeGDSCutClustering},
        {"GDS_BFS_DISTANCE", makeGDSBfsDistance},
    };
    return catalog;
}

void registerGDSFunctions(main::Database& database) {
    auto catalog = database.getCatalog();
    for (const auto& entry : getGDSCatalog()) {
        if (catalog->containsFunction(&transaction::DUMMY_TRANSACTION, entry.name,
                false /*isInternal*/)) {
            continue;
        }
        catalog->addFunction(&transaction::DUMMY_TRANSACTION,
            catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY, entry.name, entry.makeFunctionSet(),
            false /*isInternal*/);
    }
}

} // namespace algo_extension
} // namespace lbug
