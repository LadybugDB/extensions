#pragma once

// Stub for the icebug 13.6 k-core community search (NetworKit::SteinerKCore).
//
// The C++ API (include/networkit/scd/SteinerKCore.hpp) is batched multi-set SCD:
//   Communities communities(const std::vector<std::set<node>>& s);
// which does not fit the existing per-node builders in gds_generic_algo.h
// (makeNodeScoreFunc / makePartitionFunc / makeEmbeddingFunc). Wiring the real
// implementation needs a new SCD-shape builder (graph + seed-set list in,
// per-(query, member) rows out). Until that builder lands, this TableFunction
// exists so the CALL signature is reserved and tests have a pinning point —
// bind succeeds, tableFunc throws a clear "not yet implemented" error.
//
// Expected final shape (TBD): CALL GDS_STEINER_K_CORE('G', [seed, ...])
// returns (node, community_id, coreness) for the community of the seed set.

#include "function/function.h"

namespace lbug {
namespace algo_extension {

struct SteinerKCoreFunction {
    static constexpr const char* name = "GDS_STEINER_K_CORE";

    static function::function_set getFunctionSet();
};

struct SteinerKCoreAliasFunction {
    using alias = SteinerKCoreFunction;

    static constexpr const char* name = "STEINER_K_CORE";
};

} // namespace algo_extension
} // namespace lbug
