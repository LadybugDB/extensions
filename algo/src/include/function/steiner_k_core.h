#pragma once

// Single-seed k-core community search backed by icebug 13.6
// NetworKit::SteinerKCore::communities().
//
// CALL GDS_STEINER_K_CORE('G', [seeds]) (alias STEINER_K_CORE) returns one row
// per community member: (node, community_id, coreness). Single seed set only,
// so every member shares community_id 0 and the community's coreness k — the
// connected component of the k-core containing the seeds, for the largest such
// k. Batched multi-set queries (one community per seed set) are future work;
// they need an SCD-shape builder that does not fit the per-node make*Func
// builders in gds_generic_algo.h.

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
