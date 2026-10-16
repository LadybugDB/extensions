#pragma once

// The GDS catalog: every icebug-backed table function in one place.
//
// Each row is {CALL name, builder}: the builder is a captureless factory returning the
// row's function_set (built by makeNodeScoreFunc / makePartitionFunc / makeEmbeddingFunc
// from gds_generic_algo.h). Wiring a new NetworKit algorithm is one row here — no new file,
// no CMake edit, no extension-load edit.
//
// Row checklist:
//   - output column: "score" for scalar measures is not used; prefer the measure's name
//     ("rank", "community_id", "component_id", "distance", "embedding", ...).
//   - parameter signature: list the GDSGenericParams specs matching the NetworKit
//     constructor's defaults (see gds_param_specs.h). An empty list means no optional params.
//   - extra args: NONE (CALL f('G')), SOURCE_LIST (CALL f('G', [...])), SINGLE_SOURCE
//     (CALL f('G', src)).
//   - graph mode: UNDIRECTED unless the algorithm is direction-sensitive (BFS, ...).

#include <vector>

#include "function/function.h"

namespace lbug {
namespace main {
class Database;
} // namespace main

namespace algo_extension {

struct GDSCatalogEntry {
    const char* name;
    function::function_set (*makeFunctionSet)();
};

const std::vector<GDSCatalogEntry>& getGDSCatalog();

// Registers every catalog row on the database (mirrors ExtensionUtils::addFunc for
// runtime-built function sets).
void registerGDSFunctions(main::Database& database);

} // namespace algo_extension
} // namespace lbug
